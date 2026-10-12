#!/usr/bin/env python3
"""Build the Embench-IoT benchmarks with the Scry toolchain and run them on
the scryer simulator.

Each benchmark is compiled from its sources plus the Embench support files
and the Scry board support in embench/config/scry, then executed with
"scryer --machine-mode".  A benchmark passes when the exit code is 0, i.e.
verify_benchmark() accepted the result.  The simulator's instruction count
is reported as a crude performance figure (a second run without
--machine-mode is used to read the metrics).

Usage:
    run_embench.py [--embench <dir>] [--clang <clang>] [--scryer <scryer>]
                   [-O 2] [--gsf 1] [--timeout 600] [--metrics] [benchmarks...]

The Embench sources are cloned from https://github.com/embench/embench-iot
into third_party/embench-iot when --embench is not given.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRIPLE = "scry32-unknown-none-elf"
EXE = ".exe" if os.name == "nt" else ""
EMBENCH_REPO = "https://github.com/embench/embench-iot.git"
EMBENCH_REV = "09c2ed8c3b7008c95d08b038de4a3f6dc103ed70"

# Benchmarks that cannot run on the stock scryer simulator and why.  They are
# still built and run, but a failure is reported as XFAIL and does not fail
# the script (pass --strict to count them).
XFAIL = {
    "huffbench": "needs a 2 KB frame plus callers; scryer's stack is fixed at 4 KB",
    "wikisort": "needs 4 KB frames; scryer's stack is fixed at 4 KB",
}


def find_default(candidates):
    for c in candidates:
        if c.exists():
            return c
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--embench", default=str(ROOT / "third_party" / "embench-iot"))
    ap.add_argument("--clang")
    ap.add_argument("--scryer")
    ap.add_argument("-O", "--opt", default="2")
    ap.add_argument("--gsf", default="1", help="global scale factor")
    ap.add_argument("--warmup", default="1", help="warm-up heat")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds per run")
    ap.add_argument("--metrics", action="store_true",
                    help="also run without --machine-mode to print simulator metrics")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="count the known simulator-limited benchmarks (XFAIL) as failures")
    ap.add_argument("benchmarks", nargs="*")
    args = ap.parse_args()

    clang = pathlib.Path(args.clang) if args.clang else find_default([
        ROOT / "install" / "bin" / ("clang" + EXE),
        ROOT / "build" / "llvm" / "bin" / ("clang" + EXE)])
    scryer = pathlib.Path(args.scryer) if args.scryer else find_default([
        ROOT / "build" / "scryer" / "release" / ("scryer" + EXE),
        ROOT / "install" / "bin" / ("scryer" + EXE)])
    if not clang or not clang.exists():
        sys.exit("clang not found; pass --clang")
    if not scryer or not scryer.exists():
        sys.exit("scryer not found; pass --scryer")

    embench = pathlib.Path(args.embench)
    if not (embench / "src").exists():
        embench.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", EMBENCH_REPO, str(embench)], check=True)
        subprocess.run(["git", "-C", str(embench), "checkout", EMBENCH_REV], check=True)

    config = ROOT / "embench" / "config" / "scry"
    support = embench / "support"
    outdir = ROOT / "build" / "embench"
    outdir.mkdir(parents=True, exist_ok=True)

    names = args.benchmarks or sorted(p.name for p in (embench / "src").iterdir() if p.is_dir())
    results = []
    for name in names:
        srcdir = embench / "src" / name
        sources = sorted(srcdir.glob("*.c")) + [support / "main.c", support / "beebsc.c",
                                                config / "boardsupport.c"]
        elf = outdir / (name + ".elf")
        cmd = [str(clang), "--target=" + TRIPLE, "-O" + args.opt, "-g0",
               "-I", str(support), "-I", str(config), "-I", str(srcdir),
               f"-DWARMUP_HEAT={args.warmup}", f"-DGLOBAL_SCALE_FACTOR={args.gsf}",
               "-DHAVE_BOARDSUPPORT_H", "-ffunction-sections",
               "-fdata-sections", "-Wl,--gc-sections",
               *[str(s) for s in sources], "-o", str(elf), "-lm"]
        t0 = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {name}: compile error\n{r.stderr[-3000:]}")
            results.append((name, "compile-error", 0, 0))
            continue
        size = elf.stat().st_size
        run = subprocess.run([str(scryer), str(elf), "--target=" + TRIPLE,
                              "--machine-mode", f"--timeout={args.timeout}"],
                             capture_output=True, text=True)
        elapsed = time.time() - t0
        status = "PASS" if run.returncode == 0 else f"FAIL(exit {run.returncode})"
        if run.returncode != 0 and name in XFAIL and not args.strict:
            status = f"XFAIL(exit {run.returncode})"
        instrs = 0
        if args.metrics and run.returncode == 0:
            m = subprocess.run([str(scryer), str(elf), "--target=" + TRIPLE,
                                f"--timeout={args.timeout}"], capture_output=True, text=True)
            mm = re.search(r"InstructionReads:\s*(\d+)", m.stdout)
            instrs = int(mm.group(1)) if mm else 0
        print(f"{status:14} {name:18} elf={size:8d}B  {elapsed:6.1f}s"
              + (f"  instructions={instrs}" if instrs else ""), flush=True)
        if status.startswith("XFAIL"):
            print(f"               (expected: {XFAIL[name]})")
        elif run.returncode != 0 and run.stderr.strip():
            print(run.stderr[-2000:])
        results.append((name, status, size, instrs))
        if not args.keep and elf.exists():
            elf.unlink()

    passed = sum(1 for r in results if r[1] == "PASS")
    xfailed = sum(1 for r in results if r[1].startswith("XFAIL"))
    print(f"\n{passed}/{len(results)} benchmarks passed"
          + (f" ({xfailed} expected failures)" if xfailed else ""))
    return 0 if passed + xfailed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
