#!/usr/bin/env python3
"""Run the Scry backend's lit tests (llvm/test/CodeGen/Scry) with the
llvm-lit of the LLVM build tree.

LLVM generates build/llvm/bin/llvm-lit on Unix and llvm-lit.py on Windows;
this wrapper finds whichever exists and forwards the remaining arguments.

Usage:
    run_lit.py [--build-dir <dir>] [--llvm-src <dir>] [lit arguments...]
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=str(ROOT / "build" / "llvm"))
    ap.add_argument("--llvm-src", default=str(ROOT / "third_party" / "llvm-project"))
    args, rest = ap.parse_known_args()
    build = pathlib.Path(args.build_dir)
    lit = next((p for p in (build / "bin" / "llvm-lit", build / "bin" / "llvm-lit.py")
                if p.exists()), None)
    if lit is None:
        sys.exit(f"llvm-lit not found in {build / 'bin'}; build LLVM first")
    tests = pathlib.Path(args.llvm_src) / "llvm" / "test" / "CodeGen" / "Scry"
    if not rest:
        rest = ["-sv"]
    cmd = [sys.executable, str(lit), *rest, str(tests)]
    print("  " + " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    sys.exit(main())
