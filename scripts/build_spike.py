#!/usr/bin/env python3
"""Fetch and build the Spike RISC-V ISA simulator
(https://github.com/riscv-software-src/riscv-isa-sim), which
scripts/rv32_metrics.py uses to count the instructions of the rv32imac
comparison images.  The result is build/spike/bin/spike.

Requires git, a C++ compiler, make and the device tree compiler (`dtc`,
Ubuntu package device-tree-compiler, Homebrew dtc); Spike compiles the
device tree of the simulated machine with it at start-up.  Linux and
macOS only: Spike does not build with MSVC.

Usage:
    build_spike.py [--rev <git revision>] [--jobs N]
"""
import argparse
import multiprocessing
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = "https://github.com/riscv-software-src/riscv-isa-sim.git"
# Revision known to work with rv32_metrics.py (update deliberately).
DEFAULT_REV = "11848099"


def run(cmd, **kw):
    print("  " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rev", default=DEFAULT_REV)
    ap.add_argument("--src", default=str(ROOT / "third_party" / "riscv-isa-sim"))
    ap.add_argument("--jobs", type=int, default=multiprocessing.cpu_count())
    args = ap.parse_args()

    src = pathlib.Path(args.src)
    if not (src / "configure").exists():
        src.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", REPO, str(src)])
    run(["git", "-C", str(src), "fetch", "--all"])
    run(["git", "-C", str(src), "checkout", args.rev])
    run(["git", "-C", str(src), "submodule", "update", "--init", "--recursive"])

    prefix = ROOT / "build" / "spike"
    build = prefix / "obj"
    build.mkdir(parents=True, exist_ok=True)
    run([src.resolve() / "configure", f"--prefix={prefix}"], cwd=build)
    run(["make", f"-j{args.jobs}"], cwd=build)
    run(["make", "install"], cwd=build)
    print(f"spike built: {prefix / 'bin' / 'spike'}")


if __name__ == "__main__":
    main()
