#!/usr/bin/env python3
"""Executed instructions and bytes of rv32imac Embench images under Spike,
for comparison with the Scry figures of run_embench.py.

    rv32_metrics.py --spike <spike> [--prefix riscv64-unknown-elf-] [--json out.json] <image.elf>...

For every image the script runs Spike with instruction logging and counts,
for the whole run and for the window between the first execution of
start_trigger and the first execution of stop_trigger after it: executed
instructions, executed bytes and compressed (2-byte) instructions.  An
instruction is compressed when the low two bits of its encoding are not
11.  Static instructions and bytes are read from objdump's disassembly of
the executable sections.

The images are expected to be built from the same sources as the Scry
ones (benchmark, Embench support files, the runtime's libc) with the
GCC bare-metal toolchain, e.g.
    riscv64-unknown-elf-gcc -march=rv32imac -mabi=ilp32 -O2 -ffreestanding
    -nostdlib -ffunction-sections -fdata-sections -Wl,--gc-sections ...
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys


def symbol(nm, elf, name):
    out = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == name:
            return int(f[0], 16)
    sys.exit(f"{elf}: symbol {name} not found")


def dynamic(spike, elf, start, stop, isa):
    """Count the Spike log: lines look like 'core   0: 0x80000f2a (0x4501) li a0, 0'."""
    p = subprocess.Popen([spike, f"--isa={isa}", "-l", str(elf)], stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True)
    total = tbytes = tc16 = win = wbytes = wc16 = 0
    in_win = done = False
    for line in p.stderr:
        m = re.match(r"core\s+\d+: 0x([0-9a-f]+) \(0x([0-9a-f]+)\)", line)
        if not m:
            continue
        pc, enc = int(m.group(1), 16), int(m.group(2), 16)
        n = 2 if (enc & 3) != 3 else 4
        total += 1; tbytes += n; tc16 += n == 2
        if not done:
            if pc == start:
                in_win = True
            if in_win and pc == stop:
                in_win = False; done = True
            if in_win:
                win += 1; wbytes += n; wc16 += n == 2
    code = p.wait()
    return dict(exit=code, instructions=total, bytes=tbytes, compressed=tc16,
                window_instructions=win, window_bytes=wbytes, window_compressed=wc16)


def static(objdump, elf):
    out = subprocess.run([objdump, "-d", str(elf)], capture_output=True, text=True, check=True).stdout
    n = b = 0
    for line in out.splitlines():
        m = re.match(r"^\s*[0-9a-f]+:\s+([0-9a-f]+)\s", line)
        if m:
            n += 1
            b += len(m.group(1)) // 2
    return dict(static_instructions=n, static_bytes=b)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spike", required=True)
    ap.add_argument("--isa", default="rv32imac")
    ap.add_argument("--prefix", default="riscv64-unknown-elf-", help="binutils prefix")
    ap.add_argument("--json")
    ap.add_argument("images", nargs="+")
    args = ap.parse_args()

    results = {}
    print(f"{'benchmark':18} {'instrs':>11} {'bytes':>12} {'c16%':>5} {'static':>8} {'bytes':>8}")
    for img in args.images:
        elf = pathlib.Path(img)
        name = elf.stem
        start = symbol(args.prefix + "nm", elf, "start_trigger")
        stop = symbol(args.prefix + "nm", elf, "stop_trigger")
        d = dynamic(args.spike, elf, start, stop, args.isa)
        d.update(static(args.prefix + "objdump", elf))
        results[name] = d
        print(f"{name:18} {d['window_instructions']:11,d} {d['window_bytes']:12,d} "
              f"{100 * d['window_compressed'] / max(d['window_instructions'], 1):5.1f} "
              f"{d['static_instructions']:8,d} {d['static_bytes']:8,d}"
              + ("" if d["exit"] == 0 else f"  (exit {d['exit']})"))
    tot = sum(r["window_instructions"] for r in results.values())
    tb = sum(r["window_bytes"] for r in results.values())
    print(f"{'total':18} {tot:11,d} {tb:12,d}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
