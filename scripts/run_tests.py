#!/usr/bin/env python3
"""Compile the C programs under tests/c with the Scry toolchain, run them on
the scryer simulator and compare the exit code with the "// expect-exit: N"
annotation in each file (default 0).

Usage:
    run_tests.py [--clang <clang>] [--scryer <scryer>] [-O 0 1 2] [tests...]

Defaults: clang from build/llvm/bin (or install/bin), scryer from
build/scryer/release.  Returns non-zero if any test fails.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRIPLE = "scry32-unknown-none-elf"
EXE = ".exe" if os.name == "nt" else ""


def find_default(candidates):
    for c in candidates:
        if c.exists():
            return c
    return None


def warn_if_runtime_stale(clang):
    """The runtime is compiled by the Scry clang; a clang newer than the
    installed crt0.o means the runtime (and every link) carries old code."""
    crt0 = clang.parent.parent / "lib" / "clang-runtimes" / TRIPLE / "lib" / "crt0.o"
    if crt0.exists() and crt0.stat().st_mtime < clang.resolve().stat().st_mtime:
        print(f"warning: {crt0} is older than {clang}; rerun scripts/build_runtime.py",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang")
    ap.add_argument("--scryer")
    ap.add_argument("-O", "--opt", nargs="+", default=["0", "2"],
                    help="optimisation levels to test (default: 0 2)")
    ap.add_argument("--timeout", type=int, default=120, help="seconds per run")
    ap.add_argument("--keep", action="store_true", help="keep build outputs")
    ap.add_argument("--cflags", action="append", default=[],
                    help="extra compiler flag (repeatable; use --cflags=-mllvm --cflags=-x)")
    ap.add_argument("tests", nargs="*", help="test names or paths (default: all)")
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
        sys.exit("scryer not found; pass --scryer (see scripts/build_scryer.py)")
    warn_if_runtime_stale(clang)

    tests = [pathlib.Path(t) if "/" in t or t.endswith(".c") else ROOT / "tests" / "c" / (t + ".c")
             for t in args.tests] or sorted((ROOT / "tests" / "c").glob("*.c"))
    outdir = ROOT / "build" / "tests"
    outdir.mkdir(parents=True, exist_ok=True)

    failures = []
    for src in tests:
        text = src.read_text()
        m = re.search(r"expect-exit:\s*(\d+)", text)
        expected = int(m.group(1)) if m else 0
        for opt in args.opt:
            name = f"{src.stem}-O{opt}"
            elf = outdir / (name + ".elf")
            cmd = [str(clang), "--target=" + TRIPLE, "-O" + opt, "-g0",
                   *args.cflags, str(src), "-o", str(elf)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL {name}: compile error\n{r.stderr}")
                failures.append(name)
                continue
            run = subprocess.run([str(scryer), str(elf), "--target=" + TRIPLE,
                                  "--machine-mode", f"--timeout={args.timeout}"],
                                 capture_output=True, text=True)
            code = run.returncode
            if code != expected:
                print(f"FAIL {name}: exit code {code}, expected {expected}")
                if run.stdout.strip():
                    print(run.stdout)
                if run.stderr.strip():
                    print(run.stderr)
                failures.append(name)
            else:
                print(f"PASS {name}")
            if not args.keep and elf.exists():
                elf.unlink()

    total = len(tests) * len(args.opt)
    print(f"\n{total - len(failures)}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
