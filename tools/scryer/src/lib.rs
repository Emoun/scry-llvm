//! The scryer driver with code-generation metrics.
//!
//! This is scryer's `lib.rs` (revision 6fc6ac9d) plus a metrics layer: every
//! executed instruction is decoded from a snapshot of the loaded image and
//! counted per mnemonic, branch/call/ret trigger distances and load output
//! references are histogrammed, and the operand queues are measured after
//! every step.  An optional window between two symbols is reported too.

use clap::Parser;
use num_bigint::{BigInt, BigUint, Sign};
use object::{
    elf::PT_LOAD,
    read::elf::{ElfFile, FileHeader, ProgramHeader},
    Object, ObjectSegment, ObjectSymbol,
};
use regex::Regex;
use scry_asm::Assemble;
use scry_isa::{CallVariant, Instruction};
use scry_sim::{
    Block, BlockedMemory, CallFrameState, ExecError, ExecState, Executor, Memory, Metric,
    MetricReporter, OperandList, Scalar, StackFrame, TrackReport, Value, ValueType,
};
use std::{
    collections::{BTreeMap, HashMap},
    io::Write,
    time::Instant,
};

#[derive(clap::ValueEnum, Clone, Eq, PartialEq)]
enum TimeoutType {
    Instructions,
    Seconds,
}

#[derive(clap::ValueEnum, Clone, Debug)]
enum Target {
    /// Tries to detect the target automatically
    Auto,
    /// Raw binary file containing only encoded instructions
    Raw,
    /// File containing textual assembly
    Assembly,
    /// 32-bit ELF file
    Scry32UnknownNoneElf,
    /// 64-bit ELF file
    Scry64UnknownNoneElf,
}

/// Command-line arguments
#[derive(Parser)]
pub struct Cli {
    /// The path to the file to execute
    path: std::path::PathBuf,

    /// For when the simulator needs to emulate the program exactly.
    /// In this mode, the simulator's outputs (exit code, stdout, stderr) come
    /// from the simulated program.
    #[clap(short, long)]
    machine_mode: bool,

    /// The target triple of the input file
    #[clap(long)]
    #[arg(value_enum, default_value_t = Target::Auto)]
    target: Target,

    /// Input operand to the first instruction.
    /// Can be given multiple times for multiple input operands.
    /// Must give value and its type. E.g. '1u8' is an unsigned byte of value 1.
    /// Can give relative to a known label. E.g. 'entry+1u8' is an unsigned byte
    /// of value one higher than the 'entry' label.
    #[clap(short, long)]
    input: Vec<String>,

    /// Stop the simulation early.
    #[clap(long)]
    #[arg(default_value_t = 0)]
    timeout: usize,

    /// What counter should be used to timeout.
    #[clap(long)]
    #[arg(value_enum, default_value_t = TimeoutType::Seconds)]
    timeout_type: TimeoutType,

    #[clap(long)]
    debug: bool,

    /// Symbol (or hex address) whose first execution starts the measurement
    /// window.
    #[clap(long)]
    window_start: Option<String>,

    /// Symbol (or hex address) whose first execution after the start ends the
    /// measurement window.
    #[clap(long)]
    window_stop: Option<String>,

    /// Write all metrics as JSON to this file.
    #[clap(long)]
    json: Option<std::path::PathBuf>,

    /// Print every executed instruction with its ready operands (to stderr).
    #[clap(long)]
    trace: bool,

    /// Stop tracing after this many instructions (0 = unlimited).
    #[clap(long)]
    #[arg(default_value_t = 0)]
    trace_limit: u64,
}

fn format_value(v: &Value) -> String {
    match v.get_first() {
        Scalar::Val(_) => match v.get_first().i128_value() {
            Some(x) => format!("{}:{:?}", x, v.typ),
            None => format!("?:{:?}", v.typ),
        },
        other => format!("{:?}", other),
    }
}

fn parse_input(input: &String, labels: HashMap<&str, usize>) -> Value {
    let re = Regex::new(r"^((.+)\+)?(-?\d+)(u|i)(\d+)$").unwrap();
    let caps = re.captures_iter(input.as_str()).next().unwrap();

    let bit_size: u8 = caps[5].parse().unwrap();
    let byte_size = bit_size / 8;
    let byte_size_pow_2 = byte_size.ilog2() as u8;
    let signed = &caps[4] == "i";
    let value = caps[3].as_bytes();
    let relative_label = caps.get(2);
    let add_to_value = || {
        if let Some(label) = relative_label {
            if let Some(addr) = labels.get(label.as_str()) {
                return *addr;
            }
        }
        0
    };

    let typ = if signed {
        ValueType::Int(byte_size_pow_2)
    } else {
        ValueType::Uint(byte_size_pow_2)
    };
    let (sign, mut value_bytes) = if signed {
        let mut value = BigInt::parse_bytes(&value, 10).unwrap();
        value += add_to_value();
        (value.sign(), value.to_signed_bytes_le())
    } else {
        let mut value = BigUint::parse_bytes(value, 10).unwrap();
        value += add_to_value();
        (Sign::Plus, value.to_bytes_le())
    };
    // Ensure the number of bytes fits the type
    value_bytes.resize(
        byte_size as usize,
        if sign == Sign::Minus { u8::MAX } else { 0 },
    );
    assert_eq!(value_bytes.len(), byte_size as usize);

    Value::singleton_typed(typ, Scalar::Val(value_bytes.into_boxed_slice()))
}

fn value_to_string(val: &Value) -> String {
    let (mut val, typ, size_pow_2): (String, char, u8) = match val.value_type() {
        ValueType::Uint(x) => {
            let int = BigUint::from_bytes_le(val.iter().next().unwrap().bytes().unwrap());
            (int.to_string(), 'u', 2u8.pow(x as u32) * 8)
        }
        ValueType::Int(x) => {
            let int = BigInt::from_signed_bytes_le(val.iter().next().unwrap().bytes().unwrap());
            (int.to_string(), 'i', 2u8.pow(x as u32) * 8)
        }
    };
    val.push(typ);
    val.push_str(&*size_pow_2.to_string());
    val
}

fn print_metrics(tracker: &TrackReport, mut stdout: impl Write) {
    writeln!(stdout, "\n----------  Simulation Metrics  ----------").unwrap();
    for metric in Metric::all() {
        let metric_val = tracker.get_stat(metric);
        writeln!(stdout, "{:?}: {}", metric, metric_val).unwrap();
    }
}

/// The loaded program: entry address, memory, address width (power of two
/// bytes), the address range of the image and the symbol table.
struct Program {
    entry: usize,
    memory: BlockedMemory,
    address_space: u8,
    image_lo: usize,
    image_hi: usize,
    symbols: HashMap<String, usize>,
}

/// Extracts the contents of an ELF file (both 32- and 64-bit).
fn extract_elf<'a, E: FileHeader>(elf: ElfFile<'a, E, &'a [u8]>) -> Program {
    let mut mem = BlockedMemory::empty();
    let mut lo = usize::MAX;
    let mut hi = 0usize;
    for segment in elf.segments() {
        if segment.elf_program_header().p_type(elf.endian()) == PT_LOAD {
            if let Ok(data) = segment.data() {
                let addr = segment.address() as usize;
                let size = segment.size() as usize;
                lo = lo.min(addr);
                hi = hi.max(addr + size);

                mem.add_block_zeroed(addr, size);
                data.iter()
                    .enumerate()
                    .for_each(|(i, b)| mem.write_raw(addr + i, *b).unwrap())
            }
        }
    }
    let symbols = elf
        .symbols()
        .filter_map(|s| s.name().ok().map(|n| (n.to_string(), s.address() as usize)))
        .collect();
    Program {
        entry: elf.entry() as usize,
        memory: mem,
        address_space: if elf.is_64() { 3 } else { 2 },
        image_lo: if lo == usize::MAX { 0 } else { lo },
        image_hi: hi,
        symbols,
    }
}

/// Dataflow instructions only move or discard operands.
fn is_dataflow(mnemonic: &str) -> bool {
    matches!(mnemonic, "echo" | "dup" | "nop")
}

/// Counters for one measurement scope (the whole run or the window).
#[derive(Default)]
struct Stats {
    instructions: u64,
    dataflow: u64,
    opcodes: BTreeMap<&'static str, u64>,
    /// jmp trigger distances (instructions between the jmp and its trigger).
    jmp_trigger: BTreeMap<u64, u64>,
    call_trigger: BTreeMap<u64, u64>,
    ret_trigger: BTreeMap<u64, u64>,
    /// ld output references (instructions between the load and its consumer).
    load_ref: BTreeMap<u64, u64>,
    /// In-flight operand counts after each step.
    inflight_frame_sum: u64,
    inflight_frame_max: u64,
    inflight_all_sum: u64,
    inflight_all_max: u64,
    steps: u64,
}

impl Stats {
    fn count(&mut self, instr: &Instruction, ready_len: usize) {
        let m = match instr {
            Instruction::LoadStack(_, _) => "ld.s",
            Instruction::StoreStack(_) => "st.s",
            _ => instr.mnemonic(),
        };
        self.instructions += 1;
        if is_dataflow(m) {
            self.dataflow += 1;
        }
        *self.opcodes.entry(m).or_insert(0) += 1;
        match instr {
            Instruction::Jump(imm, trig) => {
                // With two operands the imm and trig fields form a 13-bit
                // trigger offset (scry_sim::handle_jump).
                let dist = if ready_len >= 2 {
                    (((imm.value as u64) & 0x7f) << 6) | ((trig.value as u64) & 0x3f)
                } else {
                    (trig.value as u64) & 0x3f
                };
                *self.jmp_trigger.entry(dist).or_insert(0) += 1;
            }
            Instruction::Call(CallVariant::Call, trig) => {
                *self.call_trigger.entry((trig.value as u64) & 0x3f).or_insert(0) += 1;
            }
            Instruction::Call(CallVariant::Ret, trig) => {
                *self.ret_trigger.entry((trig.value as u64) & 0x3f).or_insert(0) += 1;
            }
            Instruction::Load(_, offset) => {
                *self.load_ref.entry((offset.value as u64) & 0x1f).or_insert(0) += 1;
            }
            // Slot loads are built right in front of their consumer by design;
            // their distance says nothing, so they are left out of load_ref.
            Instruction::LoadStack(_, _) => {}
            _ => {}
        }
    }

    fn inflight(&mut self, frame: u64, all: u64) {
        self.steps += 1;
        self.inflight_frame_sum += frame;
        self.inflight_frame_max = self.inflight_frame_max.max(frame);
        self.inflight_all_sum += all;
        self.inflight_all_max = self.inflight_all_max.max(all);
    }

    fn mean(sum: u64, n: u64) -> f64 {
        if n == 0 {
            0.0
        } else {
            sum as f64 / n as f64
        }
    }

    fn hist_mean(h: &BTreeMap<u64, u64>) -> f64 {
        let n: u64 = h.values().sum();
        let s: u64 = h.iter().map(|(k, v)| k * v).sum();
        Self::mean(s, n)
    }

    fn write_text(&self, name: &str, mut out: impl Write) {
        writeln!(out, "----------  {}  ----------", name).unwrap();
        writeln!(out, "Instructions: {}", self.instructions).unwrap();
        writeln!(out, "DataflowInstructions: {}", self.dataflow).unwrap();
        writeln!(
            out,
            "NonDataflowInstructions: {}",
            self.instructions - self.dataflow
        )
        .unwrap();
        let mut v: Vec<_> = self.opcodes.iter().collect();
        v.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
        for (m, c) in v {
            writeln!(out, "Opcode {}: {}", m, c).unwrap();
        }
        for (label, h) in [
            ("JmpTriggerDistance", &self.jmp_trigger),
            ("CallTriggerDistance", &self.call_trigger),
            ("RetTriggerDistance", &self.ret_trigger),
            ("LoadRef", &self.load_ref),
        ] {
            let n: u64 = h.values().sum();
            let max = h.keys().max().copied().unwrap_or(0);
            writeln!(
                out,
                "{}: count={} mean={:.2} max={}",
                label,
                n,
                Self::hist_mean(h),
                max
            )
            .unwrap();
        }
        writeln!(
            out,
            "InFlightFrame: mean={:.2} max={}",
            Self::mean(self.inflight_frame_sum, self.steps),
            self.inflight_frame_max
        )
        .unwrap();
        writeln!(
            out,
            "InFlightAll: mean={:.2} max={}",
            Self::mean(self.inflight_all_sum, self.steps),
            self.inflight_all_max
        )
        .unwrap();
    }

    fn write_json(&self, out: &mut String) {
        fn hist(out: &mut String, h: &BTreeMap<u64, u64>) {
            out.push('{');
            let mut first = true;
            for (k, v) in h {
                if !first {
                    out.push(',');
                }
                first = false;
                out.push_str(&format!("\"{}\":{}", k, v));
            }
            out.push('}');
        }
        out.push_str(&format!(
            "{{\"instructions\":{},\"dataflow\":{},\"opcodes\":{{",
            self.instructions, self.dataflow
        ));
        let mut first = true;
        for (m, c) in &self.opcodes {
            if !first {
                out.push(',');
            }
            first = false;
            out.push_str(&format!("\"{}\":{}", m, c));
        }
        out.push_str("},\"jmp_trigger\":");
        hist(out, &self.jmp_trigger);
        out.push_str(",\"call_trigger\":");
        hist(out, &self.call_trigger);
        out.push_str(",\"ret_trigger\":");
        hist(out, &self.ret_trigger);
        out.push_str(",\"load_ref\":");
        hist(out, &self.load_ref);
        out.push_str(&format!(
            ",\"inflight_frame_mean\":{:.3},\"inflight_frame_max\":{},\"inflight_all_mean\":{:.3},\"inflight_all_max\":{},\"steps\":{}}}",
            Self::mean(self.inflight_frame_sum, self.steps),
            self.inflight_frame_max,
            Self::mean(self.inflight_all_sum, self.steps),
            self.inflight_all_max,
            self.steps
        ));
    }
}

fn resolve_symbol(spec: &Option<String>, symbols: &HashMap<String, usize>) -> Option<usize> {
    let s = spec.as_ref()?;
    if let Some(hex) = s.strip_prefix("0x") {
        return usize::from_str_radix(hex, 16).ok();
    }
    match symbols.get(s) {
        Some(a) => Some(*a),
        None => {
            eprintln!("scryer-metrics: symbol '{}' not found", s);
            None
        }
    }
}

fn queue_len(q: &HashMap<usize, OperandList>) -> u64 {
    q.values().map(|l| l.len() as u64).sum()
}

pub fn run(args: Cli, mut stdout: impl Write, _stderr: impl Write) -> i32 {
    let contents = std::fs::read(&args.path).unwrap();
    let contents_len = contents.len();

    let program = match args.target {
        Target::Auto => {
            unimplemented!()
        }
        Target::Raw => Program {
            entry: 0,
            memory: BlockedMemory::new(contents.into_iter(), 0),
            address_space: 2,
            image_lo: 0,
            image_hi: contents_len,
            symbols: HashMap::new(),
        },
        Target::Assembly => {
            // File is in textual assembly, assemble it
            let text = String::from_utf8(contents).unwrap();
            let program = scry_asm::Raw::assemble(std::iter::once(text.as_str())).unwrap();
            let len = program.len();
            Program {
                entry: 0,
                memory: BlockedMemory::new(program.into_iter(), 0),
                address_space: 2,
                image_lo: 0,
                image_hi: len,
                symbols: HashMap::new(),
            }
        }
        Target::Scry32UnknownNoneElf => {
            if let object::File::Elf32(elf) = object::File::parse(&*contents).unwrap() {
                extract_elf(elf)
            } else {
                unimplemented!()
            }
        }
        Target::Scry64UnknownNoneElf => {
            if let object::File::Elf64(elf) = object::File::parse(&*contents).unwrap() {
                extract_elf(elf)
            } else {
                unimplemented!()
            }
        }
    };
    let Program {
        entry,
        mut memory,
        address_space,
        image_lo,
        image_hi,
        symbols,
    } = program;

    // Ready inputs
    let mut op_queue = HashMap::new();
    if !args.input.is_empty() {
        let mut ops: Vec<_> = args
            .input
            .iter()
            .map(|s| {
                let labels = [("entry", entry)].into_iter().collect();
                parse_input(s, labels)
            })
            .collect();
        op_queue.insert(0, OperandList::new(ops.remove(0), ops));
    }

    let stack_base = 1 << 16;
    let stack_buffer = 1 << 12;
    let base_stack = StackFrame {
        block: Block {
            address: stack_base,
            size: 0,
        },
        base_size: 0,
    };
    let original_state = ExecState {
        addr_space: address_space,
        address: entry,
        frame: CallFrameState {
            ret_addr: 0,
            branches: HashMap::new(),
            op_queue,
            stack: base_stack.clone(),
        },
        frame_stack: vec![CallFrameState {
            ret_addr: 0,
            branches: HashMap::new(),
            op_queue: HashMap::new(),
            stack: base_stack,
        }],
        stack_buffer,
        more_depth: 1, // Total depth = 4
    };
    let mut tracker = TrackReport::new();
    if args.debug {
        dbg!(&original_state);
    }

    // Add stack memery block
    memory.add_block((0..stack_buffer).map(|_| 0), stack_base);

    // Snapshot of the loaded image for decoding executed instructions (code
    // never changes at run time).
    let snapshot: Vec<Option<u8>> = (image_lo..image_hi)
        .map(|a| memory.read_raw(a).ok())
        .collect();
    let decode = |addr: usize| -> Option<Instruction> {
        if addr < image_lo || addr + 1 >= image_hi {
            return None;
        }
        match (snapshot[addr - image_lo], snapshot[addr - image_lo + 1]) {
            (Some(lo), Some(hi)) => Some(Instruction::decode(u16::from_le_bytes([lo, hi]))),
            _ => None,
        }
    };
    let win_start = resolve_symbol(&args.window_start, &symbols);
    let win_stop = resolve_symbol(&args.window_stop, &symbols);
    let mut total = Stats::default();
    let mut window = Stats::default();
    let mut in_window = false;
    let mut window_done = false;

    // The executor's constructor executes the entry instruction itself:
    // count it before the loop.
    if let Some(instr) = decode(entry) {
        let ready = original_state.frame.op_queue.get(&0).map_or(0, |l| l.len());
        total.count(&instr, ready);
    }
    let mut res =
        Executor::<BlockedMemory, _>::from_state(&original_state, &mut memory).step(&mut tracker);

    let start_time = Instant::now();
    while res.is_ok() {
        let exec = res.unwrap();
        let state = exec.state();
        if args.debug {
            dbg!(&state);
        }

        if state.frame_stack.len() == 0 {
            // Done
            if let Some(returned_values) = state.frame.op_queue.get(&0) {
                if args.machine_mode {
                    // Return the integer value of the first return operand or 123 if unavailable
                    return returned_values
                        .iter()
                        .next()
                        .unwrap()
                        .get_first()
                        .bytes()
                        .map_or(123, |b| b[0] as i32);
                } else {
                    // Pretty print the returned operands
                    writeln!(stdout, "----------  Returned Operands  ----------").unwrap();
                    for val in returned_values.iter() {
                        let val_str = value_to_string(val);
                        write!(stdout, "{}, ", val_str).unwrap();
                    }

                    print_metrics(&tracker, &mut stdout);
                    writeln!(stdout).unwrap();
                    total.write_text("Code Metrics (whole run)", &mut stdout);
                    if win_start.is_some() {
                        window.write_text("Code Metrics (window)", &mut stdout);
                    }
                    if let Some(path) = &args.json {
                        let mut js = String::from("{\"whole\":");
                        total.write_json(&mut js);
                        js.push_str(",\"window\":");
                        window.write_json(&mut js);
                        js.push_str(",\"metrics\":{");
                        let mut first = true;
                        for metric in Metric::all() {
                            if !first {
                                js.push(',');
                            }
                            first = false;
                            js.push_str(&format!(
                                "\"{:?}\":{}",
                                metric,
                                tracker.get_stat(metric)
                            ));
                        }
                        js.push_str("}}\n");
                        std::fs::write(path, js).unwrap();
                    }
                    return 0;
                }
            }
            // Failure
            res = Err(ExecError::Err("No operands returned by simulation.".into()));
            continue;
        }

        // Measure the instruction about to execute and the queue state.
        let a = state.address;
        let ready = state.frame.op_queue.get(&0).map_or(0, |l| l.len());
        if !window_done {
            if Some(a) == win_start {
                in_window = true;
            }
            if in_window && Some(a) == win_stop {
                in_window = false;
                window_done = true;
            }
        }
        let frame_inflight = queue_len(&state.frame.op_queue);
        let all_inflight = frame_inflight
            + state
                .frame_stack
                .iter()
                .map(|f| queue_len(&f.op_queue))
                .sum::<u64>();
        if let Some(instr) = decode(a) {
            total.count(&instr, ready);
            if in_window {
                window.count(&instr, ready);
            }
            if args.trace && (args.trace_limit == 0 || total.instructions < args.trace_limit) {
                let ops: Vec<String> = state
                    .frame
                    .op_queue
                    .get(&0)
                    .map(|l| l.iter().map(format_value).collect())
                    .unwrap_or_default();
                let sym = symbols
                    .iter()
                    .filter(|(_, &v)| v <= a)
                    .max_by_key(|(_, &v)| v)
                    .map(|(n, &v)| format!("{}+{}", n, (a - v) / 2))
                    .unwrap_or_default();
                eprintln!("{:#x} {:<24} {:<32} [{}]", a, sym, format!("{:?}", instr), ops.join(", "));
            }
        }
        total.inflight(frame_inflight, all_inflight);
        if in_window {
            window.inflight(frame_inflight, all_inflight);
        }

        if args.timeout > 0
            && ((args.timeout_type == TimeoutType::Instructions
                && tracker.get_stat(Metric::InstructionReads) == args.timeout)
                | (args.timeout_type == TimeoutType::Seconds
                    && start_time.elapsed().as_secs() > args.timeout as u64))
        {
            if !args.machine_mode {
                writeln!(stdout, "----------  Timeout  ----------").unwrap();
                print_metrics(&tracker, &mut stdout);
            }
            return 123;
        }

        res = exec.step(&mut tracker);
    }
    // Implicit failure
    match res {
        Err(err) => {
            if !args.machine_mode {
                writeln!(stdout, "----------  Error  ----------").unwrap();
                writeln!(stdout, "{:?}", err).unwrap();
                print_metrics(&tracker, &mut stdout);
            }
        }
        Ok(_) => unreachable!(),
    }
    123
}
