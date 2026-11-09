#!/usr/bin/env python3
"""Static view of a linked Scry image: decode every instruction of the
executable sections, classify it, and report the static instruction mix,
the output references and trigger fields as written in the code and, with
an execution profile, the execution-weighted mix, code coverage and hotness.

    scry_static.py <image.elf> [--profile p.json] [--json out.json] [--dump]

Every instruction is a 16-bit little-endian word; the encodings follow the
target description (llvm/lib/Target/Scry/ScryInstrInfo.td).  Classes:

    control   jmp, call, ret
    dataflow  echo, dup, nop            (the definition used by scry_metrics.py)
    alu       add sub mul div sh and or xor eq lt gt isnar narto
    cast      cast
    pick      pick
    const     const, grow
    memory    ld =>N, st                (addressed loads and stores)
    frame     ld [i], st [i], saddr     (private stack frame slots)
    stack     rsrv, free
    trap      trap

The output reference of a producing instruction is the number of
instructions skipped before the consumer (0 = the next instruction); an
instruction whose output always goes to the next instruction counts as 0.
The trigger field of a jmp is its 6-bit near trigger distance; far-form
jumps combine it with the target field at run time, which only the dynamic
figures of scry_metrics.py account for.

--profile takes the per-address execution counts written by
scry_metrics.py --profile (run_embench.py --metrics --profile).  Coverage is
the share of static instructions executed at least once; hot50/90/99 is the
smallest number of static instructions that account for that share of the
executed instructions.
"""
import argparse
import collections
import json
import struct
import sys

CLASS_OF = {
    "jmp": "control", "call": "control", "ret": "control",
    "echo": "dataflow", "dup": "dataflow", "nop": "dataflow",
    "add": "alu", "sub": "alu", "mul": "alu", "div": "alu", "sh": "alu", "and": "alu",
    "or": "alu", "xor": "alu", "eq": "alu", "lt": "alu", "gt": "alu", "isnar": "alu",
    "narto": "alu",
    "cast": "cast", "pick": "pick", "const": "const", "grow": "const",
    "ld": "memory", "st": "memory", "ld.s": "frame", "st.s": "frame", "saddr": "frame",
    "rsrv": "stack", "free": "stack", "trap": "trap",
}
CLASSES = ("control", "dataflow", "alu", "cast", "pick", "const", "memory", "frame", "stack", "trap")

ALU1 = {(0, 0): "eq", (7, 0): "add", (0, 1): "and", (7, 1): "sub", (0, 2): "lt", (7, 2): "gt",
        (0, 3): "or", (7, 3): "xor", (0, 4): "isnar", (7, 4): "narto"}
ALU2 = {0: "add", 1: "sub", 2: "sh", 4: "mul", 5: "div"}


def decode(w):
    """Decode one 16-bit word.

    Returns (op, outputs, trigger) where op is the mnemonic ("ld.s" and
    "st.s" for the stack-slot forms), outputs the list of output references
    the instruction carries (empty for instructions without an output) and
    trigger the trigger field of jmp/call/ret (None otherwise); or None for
    a word that is not an instruction.
    """
    if w == 0:
        return "trap", [], None
    if w == 0x4000:
        return "nop", [], None
    if w == 0x8000:
        return "st", [], None
    if w & 0xF == 0x1:  # ALU
        if w & 0x8000:
            return None
        ref, mod, func = (w >> 10) & 0x1F, (w >> 7) & 7, (w >> 4) & 7
        if (mod, func) in ALU1:
            return ALU1[(mod, func)], [ref], None
        if func in ALU2 and 1 <= mod <= 6:
            outs = [ref] + ([0] if mod in (3, 4) else [])  # HighNext / LowNext
            return ALU2[func], outs, None
        return None
    if w & 0x1F in (0b01001, 0b11001):  # echo / dup
        op = "echo" if w & 0x1F == 0b01001 else "dup"
        outs = [(w >> 10) & 0x1F, (w >> 5) & 0x1F] + ([0] if w & 0x8000 else [])
        return op, outs, None
    if w & 0x3F == 0b010010:  # long echo
        return "echo", [w >> 6], None
    if w & 0x3FF == 0b10:  # pick
        if w & 0x8000:
            return None
        return "pick", [(w >> 10) & 0x1F], None
    if w & 0x3F == 0b100010:  # cast / ld
        return ("cast" if w & 0x8000 else "ld"), [(w >> 10) & 0x1F], None
    if w & 0x3F == 0b100000:  # ld.s
        if w & 0x8000:
            return None
        return "ld.s", [0], None
    if w & 0x1F == 0b10000:  # const
        return "const", [0], None
    if w & 7 == 0b100:  # jmp
        return "jmp", [], (w >> 10) & 0x3F
    if w & 0xFF == 0:
        if w >> 14 == 3:  # rsrv / free
            return ("free" if w & 0x100 else "rsrv"), [], None
        return None
    if w & 0xFF == 0x80:
        if w & 0x83FF == 0x080:
            return "st.s", [], None
        if w & 0x3FF == 0x180:
            return "call", [0], (w >> 10) & 0x3F
        if w & 0x3FF == 0x380:
            return "ret", [], (w >> 10) & 0x3F
        return None
    if w & 0xFF == 0x40:  # saddr
        if w & 0x8000:
            return None
        return "saddr", [0], None
    if w & 0xFF == 0xC0:  # grow
        return "grow", [0], None
    return None


def exec_sections(path):
    """[(address, bytes)] of the PROGBITS + SHF_EXECINSTR sections."""
    with open(path, "rb") as f:
        data = f.read()
    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    out = []
    for i in range(shnum):
        _, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from(
            "<IIIIII", data, shoff + i * shentsize)
        if sh_type == 1 and sh_flags & 0x4:
            out.append((sh_addr, data[sh_offset:sh_offset + sh_size]))
    return out


def analyse(path, profile=None, dump=None):
    instrs = []  # (address, op, outputs, trigger)
    undecodable = 0
    static_bytes = 0
    for addr, blob in exec_sections(path):
        static_bytes += len(blob)
        for i in range(0, len(blob) - 1, 2):
            w = blob[i] | (blob[i + 1] << 8)
            d = decode(w)
            if d is None:
                undecodable += 1
                if dump:
                    dump.write(f"{addr + i:#x}: {w:#06x} ???\n")
                continue
            instrs.append((addr + i,) + d)
            if dump:
                dump.write(f"{addr + i:#x}: {w:#06x} {d[0]}"
                           + (" " + " ".join(f"=>{r}" for r in d[1]) if d[1] else "")
                           + (f" trig={d[2]}" if d[2] is not None else "") + "\n")

    def hist():
        return collections.Counter()
    res = {"static_instructions": len(instrs), "static_bytes": static_bytes,
           "undecodable": undecodable}
    classes, opcodes = hist(), hist()
    refs, jmp_trig, call_trig, ret_trig = hist(), hist(), hist(), hist()
    for _, op, outs, trig in instrs:
        classes[CLASS_OF[op]] += 1
        opcodes[op] += 1
        for r in outs:
            refs[r] += 1
        if trig is not None:
            {"jmp": jmp_trig, "call": call_trig, "ret": ret_trig}[op][trig] += 1
    res.update(classes={c: classes.get(c, 0) for c in CLASSES}, opcodes=dict(sorted(opcodes.items())),
               ref_static={str(k): v for k, v in sorted(refs.items())},
               jmp_trigger_field={str(k): v for k, v in sorted(jmp_trig.items())},
               call_trigger_field={str(k): v for k, v in sorted(call_trig.items())},
               ret_trigger_field={str(k): v for k, v in sorted(ret_trig.items())})

    if profile is not None:
        xclasses, xopcodes, xrefs = hist(), hist(), hist()
        counts = []
        total = 0
        unknown = sum(n for a, n in profile.items())  # profile counts not matched to an instruction
        for a, op, outs, trig in instrs:
            n = profile.get(a, 0)
            if n:
                unknown -= n
                counts.append(n)
            total += n
            xclasses[CLASS_OF[op]] += n
            xopcodes[op] += n
            for r in outs:
                xrefs[r] += n
        counts.sort(reverse=True)
        hot = {}
        acc = 0
        targets = [(0.5, "hot50"), (0.9, "hot90"), (0.99, "hot99")]
        for i, n in enumerate(counts, 1):
            acc += n
            while targets and acc >= targets[0][0] * total:
                hot[targets.pop(0)[1]] = i
        for _, name in targets:
            hot[name] = len(counts)
        res.update(executed=total, profile_unmatched=unknown,
                   executed_classes={c: xclasses.get(c, 0) for c in CLASSES},
                   executed_opcodes=dict(sorted(xopcodes.items())),
                   ref_executed={str(k): v for k, v in sorted(xrefs.items())},
                   covered_instructions=len(counts),
                   coverage=round(len(counts) / max(len(instrs), 1), 4), **hot)
    return res


def hist_stats(h):
    """(mean, median) of a {value: count} histogram."""
    n = sum(h.values())
    if not n:
        return 0.0, 0
    mean = sum(int(k) * v for k, v in h.items()) / n
    acc = 0
    for k, v in sorted(h.items(), key=lambda kv: int(kv[0])):
        acc += v
        if acc * 2 >= n:
            return mean, int(k)
    return mean, 0


def write_text(res, out):
    n = res["static_instructions"]
    out.write(f"Static: {n} instructions, {res['static_bytes']} bytes, "
              f"{res['undecodable']} undecodable words\n")
    out.write("Classes: " + ", ".join(f"{c}={v} ({100.0 * v / max(n, 1):.1f}%)"
                                      for c, v in res["classes"].items() if v) + "\n")
    out.write("Opcodes: " + ", ".join(f"{k}={v}" for k, v in
                                      sorted(res["opcodes"].items(), key=lambda kv: -kv[1])) + "\n")
    m, md = hist_stats(res["ref_static"])
    out.write(f"OutputRef: mean={m:.2f} median={md}, JmpTriggerField: mean={hist_stats(res['jmp_trigger_field'])[0]:.2f}, "
              f"CallTriggerField: mean={hist_stats(res['call_trigger_field'])[0]:.2f}, "
              f"RetTriggerField: mean={hist_stats(res['ret_trigger_field'])[0]:.2f}\n")
    if "executed" in res:
        t = res["executed"]
        out.write(f"Executed: {t} (profile counts outside the code: {res['profile_unmatched']})\n")
        out.write("ExecutedClasses: " + ", ".join(f"{c}={v} ({100.0 * v / max(t, 1):.1f}%)"
                                                  for c, v in res["executed_classes"].items() if v) + "\n")
        m, md = hist_stats(res["ref_executed"])
        out.write(f"ExecutedOutputRef: mean={m:.2f} median={md}\n")
        out.write(f"Coverage: {res['covered_instructions']}/{n} ({100.0 * res['coverage']:.1f}%), "
                  f"hot50={res['hot50']} hot90={res['hot90']} hot99={res['hot99']} instructions\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--profile", help="per-address execution counts (JSON) from scry_metrics.py --profile")
    ap.add_argument("--json")
    ap.add_argument("--dump", action="store_true", help="print every decoded instruction to stderr")
    args = ap.parse_args()
    profile = None
    if args.profile:
        with open(args.profile) as f:
            profile = {int(k, 16): v for k, v in json.load(f).items()}
    res = analyse(args.image, profile, sys.stderr if args.dump else None)
    write_text(res, sys.stdout)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(res, f)
            f.write("\n")
    return 1 if res["undecodable"] else 0


if __name__ == "__main__":
    sys.exit(main())
