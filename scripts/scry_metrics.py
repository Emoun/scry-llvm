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
        # Constant groups (a const and its grows) by the class of the
        # instruction that finally consumes the value, by the class of the
        # first instruction that touches it after the group (a forwarding
        # echo/dup/const, else the consumer), and by length.
        self.const_groups = 0
        self.const_consumer = collections.Counter()
        self.const_direct = collections.Counter()
        self.const_length = collections.Counter()
        # The values the groups build (read as the const's type says), the
        # value classes per consumer class, the const types, and how many
        # instructions longer a group is than its value needs.
        self.const_value = collections.Counter()
        self.const_value_consumer = collections.defaultdict(collections.Counter)
        self.const_type = collections.Counter()
        self.const_excess = collections.Counter()
        # Operands consumed by ALU instructions, by what produced them (a
        # const/grow group, another ALU instruction, a load, a slot load, a
        # cast, an argument from the caller, a value returned by a callee),
        # by how they got there (direct, through echo, as a dup copy, chosen
        # by a pick), and the origins per ALU opcode.
        self.alu_inputs = 0
        self.alu_input = collections.Counter()
        self.alu_input_route = collections.Counter()
        self.alu_input_by_op = collections.defaultdict(collections.Counter)

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
            "const_groups": self.const_groups,
            "const_consumer": dict(sorted(self.const_consumer.items())),
            "const_direct": dict(sorted(self.const_direct.items())),
            "const_length": hist(self.const_length),
            "const_value": hist(self.const_value),
            "const_value_consumer": {c: dict(sorted(h.items())) for c, h in sorted(self.const_value_consumer.items())},
            "const_type": dict(sorted(self.const_type.items())),
            "const_excess": hist(self.const_excess),
            "alu_inputs": self.alu_inputs,
            "alu_input": dict(sorted(self.alu_input.items())),
            "alu_input_route": dict(sorted(self.alu_input_route.items())),
            "alu_input_by_op": {op: dict(sorted(c.items())) for op, c in sorted(self.alu_input_by_op.items())},
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
        out.write(f"ConstGroups: {self.const_groups}; consumer "
                  + ", ".join(f"{k}={v}" for k, v in self.const_consumer.most_common())
                  + "; first touched by " + ", ".join(f"{k}={v}" for k, v in self.const_direct.most_common()) + "\n")
        n = max(self.const_groups, 1)
        buckets = collections.Counter()
        for v, k in self.const_value.items():
            buckets[value_bucket(v)] += k
        out.write("ConstValues: " + ", ".join(f"{v}={k}" for v, k in self.const_value.most_common(8))
                  + "; " + ", ".join(f"{b}={100.0 * k / n:.1f}%" for b, k in buckets.most_common())
                  + f"; longer than needed {100.0 * (n - self.const_excess.get(0, 0)) / n:.1f}%\n")
        n = max(self.alu_inputs, 1)
        out.write(f"AluInputs: {self.alu_inputs}; from "
                  + ", ".join(f"{k}={100.0 * v / n:.1f}%" for k, v in self.alu_input.most_common())
                  + "; route " + ", ".join(f"{k}={100.0 * v / n:.1f}%" for k, v in self.alu_input_route.most_common()) + "\n")


JMP_RE = re.compile(r"jmp (-?\d+), (\d+)")
CONSUMER_CLASS = {
    "jmp": "control", "call": "control", "ret": "control",
    "echo": "dataflow", "dup": "dataflow", "nop": "dataflow",
    "add": "alu", "sub": "alu", "mul": "alu", "div": "alu", "sh": "alu", "and": "alu",
    "or": "alu", "xor": "alu", "eq": "alu", "lt": "alu", "gt": "alu", "isnar": "alu",
    "narto": "alu", "cast": "cast", "pick": "pick", "const": "const", "grow": "const",
    "saddr": "frame", "rsrv": "stack", "free": "stack", "trap": "trap",
}


BUCKETS = ("zero", "one", "minus one", "power of two", "mask", "other 1-byte", "2-byte", "3-byte",
           "4-byte", "8-byte")


def value_bucket(v):
    """The class of a constant group's value: 0, 1, -1, a power of two, a
    mask (2^k - 1), else the number of bytes it needs (a byte holds -128 to
    255: signed or unsigned, as the type allows)."""
    v = int(v)
    if v == 0:
        return "zero"
    if v == 1:
        return "one"
    if v == -1:
        return "minus one"
    if v > 1 and v & (v - 1) == 0:
        return "power of two"
    if v >= 3 and (v + 1) & v == 0:
        return "mask"
    if -128 <= v <= 255:
        return "other 1-byte"
    if -32768 <= v <= 65535:
        return "2-byte"
    if -(1 << 23) <= v < (1 << 24):
        return "3-byte"
    if -(1 << 31) <= v < (1 << 32):
        return "4-byte"
    return "8-byte"


def const_value(cv):
    """(value, bytes needed) of a finished group: cv holds [..., type, size,
    unsigned value]."""
    typ, size, u = cv[3], cv[4], cv[5]
    if typ[0] == "i" and u >> (8 * size - 1):
        v = u - (1 << 8 * size)
        needed = 1
        while not -(1 << (8 * needed - 1)) <= v < (1 << (8 * needed - 1)):
            needed += 1
    else:
        v = u
        needed = max(1, (v.bit_length() + 7) // 8)
    return v, needed


def consumer_class(op, text):
    """Instruction class as in scry_static.py; ld/st with a slot index are frame accesses."""
    if op in ("ld", "st"):
        return "frame" if "[" in text else "memory"
    return CONSUMER_CLASS.get(op, op)


def producer_class(op, text):
    """What an operand produced by this instruction is, for the ALU-input figures."""
    if op == "ld":
        return "slot" if "[" in text else "load"
    cls = CONSUMER_CLASS.get(op)
    if cls in ("alu", "const"):
        return cls
    if op == "saddr":
        return "frame"
    return op


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
    # Constant groups in flight: id -> [in window, length, first-touch class
    # or None, const type, size in bytes, unsigned value so far].
    cvals = {}
    grow_src = None  # the constant value a grow is extending in this step
    # Operands in flight by origin: id -> [producer class, route, call depth
    # when produced].  echo sets the route, pick sets it when it chooses the
    # value, and a dup copy inherits the class of the original it copies: a
    # dup forwards the originals first and then produces the copies in the
    # same order (twice when it also copies to the next instruction).
    src = {}
    dup_fwd = []  # ids forwarded by the dup of this step, in order
    dup_n = 0     # copies produced so far in this step
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
            if dup_fwd:
                dup_fwd = []
            dup_n = 0
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
                if op == "dup" and dup_fwd:
                    orig = src.get(dup_fwd[dup_n % len(dup_fwd)])
                    dup_n += 1
                    src[oid] = [orig[0] if orig else "dup", "dup", len(frame_base)]
                else:
                    src[oid] = [producer_class(op, text), "direct", len(frame_base)]
                if op == "const":  # const i32, -6
                    tok = text.split()
                    typ = tok[1].rstrip(",")
                    size = int(typ[1:]) // 8
                    imm = int(tok[-1]) & 0xFF
                    if typ[0] == "i" and imm >= 128:
                        imm |= ((1 << 8 * size) - 1) & ~0xFF
                    cvals[oid] = [in_win, 1, None, typ, size, imm]
                elif op == "grow" and grow_src is not None:  # grow 12
                    grow_src[1] += 1
                    imm = int(text.split()[-1]) & 0xFF
                    grow_src[5] = ((grow_src[5] << 8) | imm) & ((1 << 8 * grow_src[4]) - 1)
                    cvals[oid] = grow_src
                    grow_src = None
            elif k == "C":
                consumes += 1
                oid = int(line[17:line.index(")")])
                outstanding -= 1
                sv = src.pop(oid, None)
                if sv is not None and CONSUMER_CLASS.get(op) == "alu":
                    depth = len(frame_base)
                    origin = "argument" if depth > sv[2] else "return" if depth < sv[2] else sv[0]
                    for sc, active in ((whole, True), (window, in_win)):
                        if active:
                            sc.alu_inputs += 1
                            sc.alu_input[origin] += 1
                            sc.alu_input_route[sv[1]] += 1
                            sc.alu_input_by_op[op][origin] += 1
                cv = cvals.pop(oid, None)
                if cv is not None:
                    if op == "grow":
                        grow_src = cv
                    else:
                        cls = consumer_class(op, text)
                        v, needed = const_value(cv)
                        for sc, active in ((whole, True), (window, cv[0])):
                            if active:
                                sc.const_groups += 1
                                sc.const_consumer[cls] += 1
                                sc.const_direct[cv[2] or cls] += 1
                                sc.const_length[cv[1]] += 1
                                sc.const_value[v] += 1
                                sc.const_value_consumer[cls][value_bucket(v)] += 1
                                sc.const_type[cv[3]] += 1
                                sc.const_excess[max(cv[1] - needed, 0)] += 1
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
                src.pop(oid, None)
                p = pending.pop(oid, None)
                whole.discards += 1
                if p is not None and p[2]:
                    window.discards += 1
                cv = cvals.pop(oid, None)
                if cv is not None:
                    v, needed = const_value(cv)
                    for sc, active in ((whole, True), (window, cv[0])):
                        if active:
                            sc.const_groups += 1
                            sc.const_consumer["discarded"] += 1
                            sc.const_direct[cv[2] or "dataflow"] += 1
                            sc.const_length[cv[1]] += 1
                            sc.const_value[v] += 1
                            sc.const_value_consumer["discarded"][value_bucket(v)] += 1
                            sc.const_type[cv[3]] += 1
                            sc.const_excess[max(cv[1] - needed, 0)] += 1
            else:  # Forward
                whole.forwards += 1
                if in_win:
                    window.forwards += 1
                oid = int(line[17:line.index(")")])
                cv = cvals.get(oid)
                if cv is not None and cv[2] is None:
                    cv[2] = consumer_class(op, text)
                if op == "dup":
                    dup_fwd.append(oid)
                elif op == "echo" or op == "pick":
                    sv = src.get(oid)
                    if sv is not None and (op == "pick" or sv[1] == "direct"):
                        sv[1] = op
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
