#!/usr/bin/env python3
"""What the loads and stores of the rv32imac comparison images access.

    rv32_memory.py --spike <spike> --llvm-bin <dir> [--json out.json] [-j N] <image.elf>...

Spike's commit log (-l --log-commits) gives the address of every memory
access; on the benchmark window (first execution of start_trigger to
stop_trigger) each load and store is classified by the region of its
address (rodata: constant tables; global: .data/.bss; stack: between the
end of .bss and __stack_top) and by kind:

    callee-save  sp-relative save in a prologue or restore before a ret
                 (found in the disassembly: the leading sw run and the lw
                 run before each ret of every function)
    sp-direct    other sp-relative access: spills, scalar locals and
                 constant-index elements of stack arrays
    gp           gp-relative access to small data
    computed     any other base register: arrays, structs, pointers,
                 whether in global memory or on the stack

The table printed is the one of docs/perf-metrics.md ("rv32imac loads and
stores by target").  Needs the images of build_rv32.py and the LLVM tools
(llvm-objdump, llvm-nm, llvm-readelf) next to the clang that built them.
"""

import argparse
import bisect
import collections
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

DIS = re.compile(r"core\s+\d+: 0x([0-9a-f]+) \(0x([0-9a-f]+)\)\s*(\S*)\s*(.*)")
COMMIT = re.compile(r"core\s+\d+: \d+ 0x([0-9a-f]+) \(0x[0-9a-f]+\).*? mem 0x([0-9a-f]+)")
LOADS = {"lb", "lh", "lw", "lbu", "lhu", "lwsp"}
STORES = {"sb", "sh", "sw", "swsp"}
MEMOP = re.compile(r"(-?\d+|-?0x[0-9a-f]+)\((\w+)\)")


def sections(readelf, elf):
    out = subprocess.run([readelf, "-S", str(elf)], capture_output=True, text=True, check=True).stdout
    secs = {}
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", line)
        if m:
            secs[m.group(1)] = (int(m.group(2), 16), int(m.group(2), 16) + int(m.group(3), 16))
    return secs


def analyse_code(objdump, nm, elf):
    """functions [(addr, name)], callee-save site pcs, symbol values."""
    syms = {}
    for line in subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", str(elf)], capture_output=True, text=True, check=True).stdout
    funcs = []
    saves = set()
    cur = []
    def finish(block):
        if not block:
            return
        # prologue: leading run of addi sp / sw x, off(sp)
        for pc, mn, ops in block:
            if mn in ("addi", "c.addi16sp", "c.addi") and ops.startswith("sp,"):
                continue
            if mn in ("sw", "c.swsp") and "(sp)" in ops:
                saves.add(pc); continue
            break
        # epilogue: run of lw x, off(sp) / addi sp before each ret
        for i, (pc, mn, ops) in enumerate(block):
            if mn in ("ret", "c.jr", "jr") or (mn in ("j", "c.j", "tail") and False):
                j = i - 1
                while j >= 0:
                    pc2, mn2, ops2 = block[j]
                    if mn2 in ("addi", "c.addi16sp", "c.addi") and ops2.startswith("sp,"):
                        j -= 1; continue
                    if mn2 in ("lw", "c.lwsp") and "(sp)" in ops2:
                        saves.add(pc2); j -= 1; continue
                    break
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
        if m:
            finish(cur); cur = []
            funcs.append((int(m.group(1), 16), m.group(2)))
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s*(\S+)\s*(.*)$", line)
        if m and cur is not None and funcs:
            cur.append((int(m.group(1), 16), m.group(2), m.group(3)))
    finish(cur)
    return sorted(funcs), saves, syms


def one(spike, objdump, nm, readelf, elf):
    funcs, saves, syms = analyse_code(objdump, nm, elf)
    faddrs = [a for a, _ in funcs]
    secs = sections(readelf, elf)
    rodata = secs.get(".rodata", (0, 0))
    glob = [secs[s] for s in (".data", ".bss", ".sdata", ".sbss", ".tohost") if s in secs]
    stack = (syms["__bss_end"], syms["__stack_top"])
    start, stop = syms["start_trigger"], syms["stop_trigger"]
    p = subprocess.Popen([spike, "--isa=rv32imac", "-l", "--log-commits", str(elf)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    in_win = done = False
    n = 0
    last = None  # (pc, mnemonic, ops)
    cnt = collections.Counter()
    func = collections.Counter()
    for line in p.stderr:
        m = COMMIT.match(line)
        if m:
            if not in_win or last is None or int(m.group(1), 16) != last[0]:
                continue
            pc, mn, ops = last
            core = mn[2:] if mn.startswith("c.") else mn
            if core in LOADS:
                k = "load"
            elif core in STORES:
                k = "store"
            else:
                continue
            a = int(m.group(2), 16)
            if rodata[0] <= a < rodata[1]:
                region = "rodata"
            elif any(lo <= a < hi for lo, hi in glob):
                region = "global"
            elif stack[0] <= a < stack[1]:
                region = "stack"
            else:
                region = "other"
            mo = MEMOP.search(ops)
            base = mo.group(2) if mo else ("sp" if core.endswith("sp") else "?")
            if pc in saves:
                kind = "callee-save"
            elif base == "sp":
                kind = "sp-direct"
            elif base == "gp":
                kind = "gp"
            else:
                kind = "computed"
            cnt[(k, region, kind)] += 1
            fn = funcs[bisect.bisect_right(faddrs, pc) - 1][1]
            func[(k, fn)] += 1
            continue
        m = DIS.match(line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        if not done:
            if pc == start:
                in_win = True
            if in_win and pc == stop:
                in_win = False; done = True
        if in_win:
            n += 1
        last = (pc, m.group(3), m.group(4))
    p.wait()
    return dict(window_instructions=n,
                counts={f"{k}/{r}/{d}": v for (k, r, d), v in cnt.items()},
                func={f"{k}/{f}": v for (k, f), v in func.items()},
                callee_save_sites=len(saves))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spike", required=True); ap.add_argument("--llvm-bin", required=True)
    ap.add_argument("--json"); ap.add_argument("-j", type=int, default=2)
    ap.add_argument("images", nargs="+")
    a = ap.parse_args()
    b = a.llvm_bin
    res = {}
    with ThreadPoolExecutor(a.j) as ex:
        futs = {ex.submit(one, a.spike, b + "/llvm-objdump", b + "/llvm-nm", b + "/llvm-readelf", e): e for e in a.images}
        for f, e in futs.items():
            name = e.rsplit("/", 1)[-1][:-4]
            res[name] = f.result()
            r = res[name]
            print(f"{name:16} n={r['window_instructions']:>10,} ld/st={sum(r['counts'].values()):>9,}",
                  file=sys.stderr, flush=True)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1, sort_keys=True)
    print(markdown(res))


def markdown(res):
    """The per-benchmark table with a pooled row."""
    hdr = ["benchmark", "ld/st", "rodata", "global", "stack", "callee-save", "sp spills/locals",
           "gp", "computed"]
    rows = []
    tot = collections.Counter()
    tn = 0

    def row(name, c, n):
        t = sum(c.values())
        reg = collections.Counter()
        kind = collections.Counter()
        for k, v in c.items():
            _, region, kd = k.split("/")
            reg[region] += v
            kind[kd] += v
        pc = lambda a, b: f"{100.0 * a / b:.1f}%" if b else "-"
        return [name, pc(t, n), pc(reg["rodata"], t), pc(reg["global"], t), pc(reg["stack"], t),
                pc(kind["callee-save"], t), pc(kind["sp-direct"], t), pc(kind["gp"], t),
                pc(kind["computed"], t)]
    for name in sorted(res):
        c = res[name]["counts"]
        rows.append(row(name, c, res[name]["window_instructions"]))
        tot.update(c)
        tn += res[name]["window_instructions"]
    rows.append(row("**pooled**", tot, tn))
    out = ["| " + " | ".join(hdr) + " |", "|" + "|".join("---:" if i else "---" for i in range(len(hdr))) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


if __name__ == "__main__":
    main()
