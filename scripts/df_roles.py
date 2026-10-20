#!/usr/bin/env python3
"""Static breakdown of the dataflow instructions (echo, dup, nop) the Scry
backend emits, by role, read off the assembly comments.

    df_roles.py [--clang <path>] [--cflags <flag>]... [--profile p.json --elf image]
                <source.c or .s>...

Each C source is compiled to assembly with the given clang (default:
build/llvm/bin/clang, -O2); .s files are read as they are.  The roles are:
landing pads (and how many of them are nops), deliveries to successors,
jump conditions, call arguments and results, function entry, the first
instruction of an expansion (ordering fixups and captures) and the rest
(hops and copies inside an expansion).

With --profile (the per-address execution counts written by scryer-metrics
--profile, or run_embench.py --metrics --profile) and --elf (the linked
image), every instruction is weighted by how often it ran: a function's
instructions are two bytes each from its symbol address, so the n-th
instruction of a function sits at address + 2n.  Executed instructions
outside the given sources (the runtime library) are reported separately.
"""
import argparse
import collections
import json
import os
import pathlib
import re
import signal
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRIPLE = "scry32-unknown-none-elf"
EXE = ".exe" if os.name == "nt" else ""


def classify(line):
    op = line.split()[0]
    comment = line.split(";", 1)[1] if ";" in line else ""
    if "landing pad" in comment:
        return "landing pad"
    if "deliver to successors" in comment:
        return "delivery"
    if "jump" in comment:
        return "jump condition"
    if "CALL" in comment:
        return "call args/result"
    if "RETURN" in comment:
        return "return"
    if "ARG_" in comment:
        return "entry"
    if comment.strip() == "":
        return "inside an expansion (hop/copy/delivery tail)"
    return "first of an expansion (fixup/capture)"


def main():
    if hasattr(signal, "SIGPIPE"):  # a closed pipe (| head) ends the output quietly
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang", default=str(ROOT / "build" / "llvm" / "bin" / ("clang" + EXE)))
    ap.add_argument("--cflags", action="append", default=[])
    ap.add_argument("--profile", help="per-address execution counts (JSON) to weight by")
    ap.add_argument("--elf", help="the linked image the profile was taken from")
    ap.add_argument("sources", nargs="+")
    args = ap.parse_args()

    profile = addresses = None
    if args.profile:
        if not args.elf:
            sys.exit("--profile needs --elf")
        with open(args.profile) as f:
            profile = {int(k, 16): v for k, v in json.load(f).items()}
        nm = pathlib.Path(args.clang).parent / ("llvm-nm" + EXE)
        out = subprocess.run([str(nm), args.elf], capture_output=True, text=True, check=True).stdout
        addresses = {}
        for line in out.splitlines():
            f = line.split()
            if len(f) == 3 and f[1] in "tTwW":
                addresses[f[2]] = int(f[0], 16)

    counts = collections.Counter()
    total = 0
    covered = 0  # profile counts attributed to the given sources
    for src in args.sources:
        if src.endswith(".s"):
            text = open(src).read()
        else:
            cmd = [args.clang, "--target=" + TRIPLE, "-O2", "-S", "-o", "-", *args.cflags, src]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"{src}: compile error\n{r.stderr[-2000:]}")
            text = r.stdout
        func = None   # (address, index of the next instruction)
        for line in text.splitlines():
            label = line.split(";")[0].rstrip()
            if profile is not None and not line.startswith("\t") and label.endswith(":") \
                    and not label.startswith("."):
                name = label[:-1]
                func = [addresses[name], 0] if name in addresses else None
            if not line.startswith("\t") or line.startswith("\t."):
                continue
            body = line[1:]
            weight = 1
            if profile is not None:
                weight = 0
                if func is not None:
                    weight = profile.get(func[0] + 2 * func[1], 0)
                    func[1] += 1
                covered += weight
            total += weight
            if re.match(r"(echo|dup|nop)\b", body):
                counts[(body.split()[0], classify(body))] += weight

    df = sum(counts.values())
    what = "executed instructions" if profile is not None else "instructions"
    print(f"{total} {what}, {df} dataflow ({100.0 * df / max(total, 1):.1f}%)")
    if profile is not None:
        rest = sum(profile.values()) - covered
        print(f"{rest} executed instructions outside the given sources (runtime)")
    for (op, role), n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"{n:8d} {100.0 * n / max(df, 1):5.1f}%  {op:4s} {role}")


if __name__ == "__main__":
    main()
