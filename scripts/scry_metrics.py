#!/usr/bin/env python3
"""Code-generation metrics of a Scry program, computed from the event stream
of the scryer simulator (scryer --print-events).

    scry_metrics.py <program.elf> [--scryer <scryer>] [--target <triple>]
                    [--window-start <symbol>] [--window-stop <symbol>]
                    [--json out.json] [--profile out.json] [--trace] [--trace-limit N]
                    [--timeout <seconds>] [--stack-base <addr|symbol>] [--stack-buffer <bytes>]

scryer is run with the events streamed into a named pipe (a temporary file
on Windows) and every step is folded into counters as it arrives, so a run
of millions of instructions needs no memory for the events.  Reported for
the whole run and for the window between the first execution of the start
symbol and the first execution of the stop symbol after it:

* executed instructions, how many of them are dataflow instructions
  (echo, dup, nop), and the count per opcode;
* jmp_trigger, call_trigger, ret_trigger: histograms of the distance, in
  instructions of the code, between a control instruction and its trigger
  (from the trigger immediate, as in the assembly), and jmp_dynamic etc.:
  the same distance in executed instructions, from the issue and trigger
  events (a callee executed in between counts);
* load_ref: histogram of the output reference of memory loads (slot loads
  are built in front of their consumer and left out), and load_dynamic:
  executed instructions from the load to the first consumer of its value;
* value_ref: executed instructions from any value's production to its
  first consumer; discards and forwards: values dropped by their receiver
  and values passed on by const/grow;
* inflight_frame_mean/max, inflight_all_mean/max: operands produced and not
  yet consumed after each step, in the current frame and in all frames;
* loads, stores and their bytes, and how many of them are stack-slot
  accesses (ld [i], st [i]); produced: operands produced; jmp_issued,
  jmp_taken, jmp_not_taken and jmp_overridden: a jmp whose condition holds
  issues a control event, one whose condition fails issues nothing (not
  taken), and an issued jump whose trigger point a later jump of the same
  frame takes over never triggers (overridden: the backend issues a block's
  unconditional jump first and a conditional one with the same trigger
  overrides it), so the taken jumps are the ones that trigger;
  call_depth_max: deepest call nesting seen.

scryer runs in --machine-mode, so its exit status is the program's result
(the low byte of the first returned operand; 123 on a timeout or error) and
it is this script's exit status too.
"""
import argparse
import collections
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import threading

DATAFLOW = ("echo", "dup", "nop")


def elf_symbols(path):
    """Name -> address of the defined symbols of a 32-bit little-endian ELF file."""
    with open(path, "rb") as f:
        data = f.read()
    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for sh in sections:
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9]
        if sh_type != 2:  # SHT_SYMTAB
            continue
        str_off = sections[sh_link][4]
        for i in range(sh_size // sh_entsize):
            name_off, value, _, _, _, shndx = struct.unpack_from("<IIIBBH", data, sh_offset + i * sh_entsize)
            if shndx == 0 or name_off == 0:
                continue
            end = data.index(b"\0", str_off + name_off)
            syms[data[str_off + name_off:end].decode()] = value
    return syms


class Scope:
    """Counters of one measurement scope (the whole run or the window)."""

    def __init__(self):
        self.instructions = 0
        self.dataflow = 0
        self.opcodes = collections.Counter()
        self.jmp_trigger = collections.Counter()
        self.call_trigger = collections.Counter()
        self.ret_trigger = collections.Counter()
        self.jmp_dynamic = collections.Counter()
        self.call_dynamic = collections.Counter()
        self.ret_dynamic = collections.Counter()
        self.load_ref = collections.Counter()
        self.load_dynamic = collections.Counter()
        self.value_ref = collections.Counter()
        self.discards = 0
        self.forwards = 0
        self.loads = self.load_bytes = self.stores = self.store_bytes = 0
        self.stack_loads = self.stack_load_bytes = self.stack_stores = self.stack_store_bytes = 0
        self.produced = 0
        self.jmp_issued = self.jmp_taken = 0
        self.call_depth_max = 0
        self.inflight_frame_sum = self.inflight_frame_max = 0
        self.inflight_all_sum = self.inflight_all_max = 0
        self.steps = 0

    def to_json(self):
        def hist(h):
            return {str(k): v for k, v in sorted(h.items())}
        return {
            "instructions": self.instructions, "dataflow": self.dataflow,
            "opcodes": dict(sorted(self.opcodes.items())),
            "jmp_trigger": hist(self.jmp_trigger), "call_trigger": hist(self.call_trigger),
            "ret_trigger": hist(self.ret_trigger), "jmp_dynamic": hist(self.jmp_dynamic),
            "call_dynamic": hist(self.call_dynamic), "ret_dynamic": hist(self.ret_dynamic),
            "load_ref": hist(self.load_ref), "load_dynamic": hist(self.load_dynamic),
            "value_ref": hist(self.value_ref), "discards": self.discards, "forwards": self.forwards,
            "loads": self.loads, "load_bytes": self.load_bytes,
            "stores": self.stores, "store_bytes": self.store_bytes,
            "stack_loads": self.stack_loads, "stack_load_bytes": self.stack_load_bytes,
            "stack_stores": self.stack_stores, "stack_store_bytes": self.stack_store_bytes,
            "produced": self.produced, "jmp_issued": self.jmp_issued, "jmp_taken": self.jmp_taken,
            "jmp_not_taken": self.opcodes.get("jmp", 0) - self.jmp_issued,
            "jmp_overridden": self.jmp_issued - self.jmp_taken,
            "call_depth_max": self.call_depth_max,
            "inflight_frame_mean": round(self.inflight_frame_sum / self.steps, 3) if self.steps else 0.0,
            "inflight_frame_max": self.inflight_frame_max,
            "inflight_all_mean": round(self.inflight_all_sum / self.steps, 3) if self.steps else 0.0,
            "inflight_all_max": self.inflight_all_max, "steps": self.steps,
        }

    def write_text(self, title, out):
        def mean(h):
            n = sum(h.values())
            return sum(k * v for k, v in h.items()) / n if n else 0.0
        out.write(f"\n----------  {title}  ----------\n")
        out.write(f"Instructions: {self.instructions}\n")
        out.write(f"Dataflow: {self.dataflow} ({100.0 * self.dataflow / max(self.instructions, 1):.1f}%)\n")
        out.write("Opcodes: " + ", ".join(f"{k}={v}" for k, v in sorted(self.opcodes.items(), key=lambda kv: -kv[1])) + "\n")
        out.write(f"JmpTrigger: mean={mean(self.jmp_trigger):.2f} (dynamic {mean(self.jmp_dynamic):.2f}), "
                  f"CallTrigger: mean={mean(self.call_trigger):.2f}, RetTrigger: mean={mean(self.ret_trigger):.2f}\n")
        out.write(f"LoadRef: mean={mean(self.load_ref):.2f} (dynamic {mean(self.load_dynamic):.2f}), "
                  f"ValueRef: mean={mean(self.value_ref):.2f}, discards={self.discards}, forwards={self.forwards}\n")
        out.write(f"Loads: {self.loads} ({self.load_bytes} bytes, {self.stack_loads} from stack slots), "
                  f"Stores: {self.stores} ({self.store_bytes} bytes, {self.stack_stores} to stack slots)\n")
        out.write(f"Produced: {self.produced} operands ({self.produced / max(self.instructions, 1):.2f} per instruction), "
                  f"Jumps taken: {self.jmp_taken}, not taken: {self.opcodes.get('jmp', 0) - self.jmp_issued}, "
                  f"overridden: {self.jmp_issued - self.jmp_taken}, "
                  f"CallDepth: max={self.call_depth_max}\n")
        out.write(f"InFlightFrame: mean={self.inflight_frame_sum / max(self.steps, 1):.2f} max={self.inflight_frame_max}\n")
        out.write(f"InFlightAll: mean={self.inflight_all_sum / max(self.steps, 1):.2f} max={self.inflight_all_max}\n")


JMP_RE = re.compile(r"jmp (-?\d+), (\d+)")
CALL_RE = re.compile(r"(?:call|ret) (\d+)")
LD_RE = re.compile(r"=>(\d+)")


def parse_stream(stream, start, stop, symbols, trace, trace_limit, profile):
    """Fold the event stream into the whole-run and window scopes."""
    whole, window = Scope(), Scope()
    in_win = done = False
    step = 0
    # Per-step state.
    op = ""; text = ""; addr = 0; consumes = 0
    # Control ids: id -> (step issued, kind, static distance).
    issued = {}
    # Operand ids produced and not yet consumed: id -> (step, is load, in window).
    pending = {}
    outstanding = 0
    frame_base = [0]
    sym_by_addr = sorted((a, n) for n, a in symbols.items()) if trace else None

    def end_step():
        nonlocal consumes
        # A jump's static trigger distance: near form (one ready operand or
        # none) from the trigger field, far form (two) from both fields.
        if op == "jmp":
            m = JMP_RE.search(text)
            if m:
                imm, trig = int(m.group(1)), int(m.group(2))
                dist = (((imm & 0x7f) << 6) | (trig & 0x3f)) if consumes >= 2 else (trig & 0x3f)
                whole.jmp_trigger[dist] += 1
                if in_win:
                    window.jmp_trigger[dist] += 1
                for cid, (s, kind, _) in issued.items():
                    if s == step and kind == "Jump":
                        issued[cid] = (s, kind, dist)
        elif op in ("call", "ret"):
            m = CALL_RE.search(text)
            if m:
                dist = int(m.group(1)) & 0x3f
                h = whole.call_trigger if op == "call" else whole.ret_trigger
                h[dist] += 1
                if in_win:
                    (window.call_trigger if op == "call" else window.ret_trigger)[dist] += 1
        frame = outstanding - frame_base[-1]
        if frame < 0:
            frame = 0
        for sc, active in ((whole, True), (window, in_win)):
            if not active:
                continue
            sc.steps += 1
            sc.inflight_frame_sum += frame
            sc.inflight_all_sum += outstanding
            if frame > sc.inflight_frame_max:
                sc.inflight_frame_max = frame
            if outstanding > sc.inflight_all_max:
                sc.inflight_all_max = outstanding
        consumes = 0

    for line in stream:
        c = line[0]
        if c == "S":  # STEP_END, Store(addr): n, StoreStack(addr,idx): n, Stack: ...
            if line[1] == "T":
                end_step()
                step += 1
            elif line[2] == "o":
                n = int(line[line.rindex(":") + 2:])
                stack = line[5] == "S"
                whole.stores += 1
                whole.store_bytes += n
                if stack:
                    whole.stack_stores += 1
                    whole.stack_store_bytes += n
                if in_win:
                    window.stores += 1
                    window.store_bytes += n
                    if stack:
                        window.stack_stores += 1
                        window.stack_store_bytes += n
            continue
        if c == "I" and line[1] == "n":  # Instruction: (0x....) text
            p = line.index(")")
            addr = int(line[16:p], 16)
            text = line[p + 2:].rstrip("\n")
            sp = text.find(" ")
            op = text[:sp] if sp > 0 else text
            if not done:
                if addr == start:
                    in_win = True
                if in_win and addr == stop:
                    in_win = False
                    done = True
            whole.instructions += 1
            whole.opcodes[op] += 1
            if op in DATAFLOW:
                whole.dataflow += 1
            if in_win:
                window.instructions += 1
                window.opcodes[op] += 1
                if op in DATAFLOW:
                    window.dataflow += 1
                if profile is not None:
                    profile[addr] += 1
            if op == "ld" and "[" not in text:  # a memory load with an output reference
                m = LD_RE.search(text)
                if m:
                    whole.load_ref[int(m.group(1))] += 1
                    if in_win:
                        window.load_ref[int(m.group(1))] += 1
            if trace and (trace_limit == 0 or whole.instructions <= trace_limit):
                import bisect
                i = bisect.bisect_right(sym_by_addr, (addr, "\xff")) - 1
                name = sym_by_addr[i][1] if i >= 0 else "?"
                sys.stderr.write(f"{whole.instructions:9d} {addr:#x} {name}: {text}\n")
            continue
        if c == "O":  # Operand: Produce(id,type) | Consume(id) | Forward(id) | Discard(id)
            k = line[9]
            if k == "P":
                i = line.index("(") + 1
                oid = int(line[i:line.index(",", i)])
                outstanding += 1
                whole.produced += 1
                if in_win:
                    window.produced += 1
                pending[oid] = (step, op == "ld" and "[" not in text, in_win)
            elif k == "C":
                consumes += 1
                oid = int(line[17:line.index(")")])
                outstanding -= 1
                p = pending.pop(oid, None)
                if p is not None:
                    d = step - p[0] - 1
                    whole.value_ref[d] += 1
                    if p[1]:
                        whole.load_dynamic[d] += 1
                    if p[2]:
                        window.value_ref[d] += 1
                        if p[1]:
                            window.load_dynamic[d] += 1
            elif k == "D":
                oid = int(line[17:line.index(")")])
                outstanding -= 1
                p = pending.pop(oid, None)
                whole.discards += 1
                if p is not None and p[2]:
                    window.discards += 1
            else:  # Forward
                whole.forwards += 1
                if in_win:
                    window.forwards += 1
            continue
        if c == "I":  # Issue: Kind(id)
            k = line.index("(")
            kind = line[7:k]
            issued[int(line[k + 1:line.index(")")])] = (step, kind, None)
            if kind == "Jump":
                whole.jmp_issued += 1
                if in_win:
                    window.jmp_issued += 1
            continue
        if c == "T":  # Trigger: Kind(id)
            k = line.index("(")
            cid = int(line[k + 1:line.index(")")])
            kind = line[9:k]
            rec = issued.pop(cid, None)
            if kind == "Jump":
                whole.jmp_taken += 1
                if in_win:
                    window.jmp_taken += 1
            if rec is not None:
                d = step - rec[0]
                h = {"Jump": whole.jmp_dynamic, "Call": whole.call_dynamic, "Return": whole.ret_dynamic}.get(kind)
                if h is not None:
                    h[d] += 1
                    if in_win:
                        {"Jump": window.jmp_dynamic, "Call": window.call_dynamic, "Return": window.ret_dynamic}[kind][d] += 1
            if kind == "Call":
                frame_base.append(outstanding)
                depth = len(frame_base) - 1
                if depth > whole.call_depth_max:
                    whole.call_depth_max = depth
                if in_win and depth > window.call_depth_max:
                    window.call_depth_max = depth
            elif kind == "Return" and len(frame_base) > 1:
                frame_base.pop()
            continue
        if c == "L":  # Load(addr): n | LoadStack(addr,idx): n
            n = int(line[line.rindex(":") + 2:])
            stack = line[4] == "S"
            whole.loads += 1
            whole.load_bytes += n
            if stack:
                whole.stack_loads += 1
                whole.stack_load_bytes += n
            if in_win:
                window.loads += 1
                window.load_bytes += n
                if stack:
                    window.stack_loads += 1
                    window.stack_load_bytes += n
            continue
    return whole, window


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("program")
    ap.add_argument("--scryer", help="scryer binary (default: build/scryer/release/scryer)")
    ap.add_argument("--target", default="scry32-unknown-none-elf")
    ap.add_argument("--window-start")
    ap.add_argument("--window-stop")
    ap.add_argument("--json")
    ap.add_argument("--profile", help="write the execution count of every address in the window as JSON")
    ap.add_argument("--trace", action="store_true", help="print every executed instruction to stderr")
    ap.add_argument("--trace-limit", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=0, help="seconds (0 = none)")
    ap.add_argument("--stack-base")
    ap.add_argument("--stack-buffer", type=int)
    args = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    scryer = pathlib.Path(args.scryer) if args.scryer else root / "build" / "scryer" / "release" / ("scryer.exe" if os.name == "nt" else "scryer")
    if not scryer.exists():
        sys.exit(f"scryer not found at {scryer}; pass --scryer")
    symbols = elf_symbols(args.program)

    def resolve(spec):
        if spec is None:
            return None
        if spec in symbols:
            return symbols[spec]
        return int(spec, 0)
    start, stop = resolve(args.window_start), resolve(args.window_stop)

    cmd = [str(scryer), args.program, "--target=" + args.target, "--machine-mode"]
    if args.timeout:
        cmd += [f"--timeout={args.timeout}"]
    if args.stack_base:
        cmd += ["--stack-base", args.stack_base]
    if args.stack_buffer:
        cmd += ["--stack-buffer", str(args.stack_buffer)]

    profile = collections.Counter() if args.profile else None
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "events")
        use_fifo = hasattr(os, "mkfifo")
        if use_fifo:
            os.mkfifo(path)
        cmd.append("--print-events=" + path)
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        result = {}
        if use_fifo:
            # scryer opens the pipe for writing once it starts; read until it closes it.
            def pump():
                result["out"], result["err"] = proc.communicate()
            t = threading.Thread(target=pump)
            t.start()
            with open(path, "r", buffering=1 << 20) as stream:
                whole, window = parse_stream(stream, start, stop, symbols, args.trace, args.trace_limit, profile)
            t.join()
        else:
            result["out"], result["err"] = proc.communicate()
            with open(path, "r", buffering=1 << 20) as stream:
                whole, window = parse_stream(stream, start, stop, symbols, args.trace, args.trace_limit, profile)

    # Machine mode: the exit status is the program's result (123 on a
    # timeout or a simulation error, 101 when the simulator panics).
    code = proc.returncode
    if code in (101, 123):
        sys.stderr.write(f"scryer: exit {code}\n" + result["out"][-2000:] + result["err"][-2000:])

    whole.write_text("Code Metrics (whole run)", sys.stdout)
    if start is not None:
        window.write_text("Code Metrics (window)", sys.stdout)
    sys.stdout.write(f"\nProgram result: {code}\n")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"whole": whole.to_json(), "window": window.to_json()}, f)
            f.write("\n")
    if args.profile:
        with open(args.profile, "w") as f:
            json.dump({f"{a:#x}": n for a, n in sorted(profile.items())}, f)
            f.write("\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
