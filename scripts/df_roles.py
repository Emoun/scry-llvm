#!/usr/bin/env python3
"""Static breakdown of the dataflow instructions (echo, dup, nop) the Scry
backend emits, by role, read off the assembly comments.

    df_roles.py [--clang <path>] [--cflags <flag>]... <source.c or .s>...

Each C source is compiled to assembly with the given clang (default:
build/llvm/bin/clang, -O2); .s files are read as they are.  The roles are:
landing pads (and how many of them are nops), deliveries to successors,
jump conditions, call arguments and results, function entry, the first
instruction of an expansion (ordering fixups and captures) and the rest
(hops and copies inside an expansion).
"""
import argparse
import collections
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
    ap.add_argument("sources", nargs="+")
    args = ap.parse_args()

    counts = collections.Counter()
    total = 0
    for src in args.sources:
        if src.endswith(".s"):
            text = open(src).read()
        else:
            cmd = [args.clang, "--target=" + TRIPLE, "-O2", "-S", "-o", "-", *args.cflags, src]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"{src}: compile error\n{r.stderr[-2000:]}")
            text = r.stdout
        for line in text.splitlines():
            if not line.startswith("\t") or line.startswith("\t."):
                continue
            body = line[1:]
            total += 1
            if re.match(r"(echo|dup|nop)\b", body):
                counts[(body.split()[0], classify(body))] += 1

    df = sum(counts.values())
    print(f"{total} instructions, {df} dataflow ({100.0 * df / max(total, 1):.1f}%)")
    for (op, role), n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"{n:8d} {100.0 * n / max(df, 1):5.1f}%  {op:4s} {role}")


if __name__ == "__main__":
    main()
