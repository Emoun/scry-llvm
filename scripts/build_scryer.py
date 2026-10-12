#!/usr/bin/env python3
"""Fetch and build the scryer simulator (https://github.com/Scry-arch/scryer)
with cargo.  The result is build/scryer/release/scryer (scryer.exe on
Windows).  Requires git and a Rust toolchain (https://rustup.rs).

Usage:
    build_scryer.py [--rev <git revision>] [--install <dir>]
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = "https://github.com/Scry-arch/scryer.git"
# Revision known to work with this toolchain (update deliberately).
DEFAULT_REV = "6fc6ac9d8fa452d306049692bd8ac1ab02c2e69e"


def run(cmd, **kw):
    print("  " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rev", default=DEFAULT_REV)
    ap.add_argument("--src", default=str(ROOT / "third_party" / "scryer"))
    ap.add_argument("--install", help="copy the binary into this directory")
    args = ap.parse_args()

    src = pathlib.Path(args.src)
    if not (src / "Cargo.toml").exists():
        src.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", REPO, str(src)])
    run(["git", "-C", str(src), "fetch", "--all"])
    run(["git", "-C", str(src), "checkout", args.rev])

    target_dir = ROOT / "build" / "scryer"
    run(["cargo", "build", "--release", "--manifest-path", str(src / "Cargo.toml"),
         "--target-dir", str(target_dir)])
    exe = target_dir / "release" / ("scryer.exe" if os.name == "nt" else "scryer")
    print(f"scryer built: {exe}")
    if args.install:
        dst = pathlib.Path(args.install)
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copy(exe, dst / exe.name)
        print(f"installed to {dst / exe.name}")


if __name__ == "__main__":
    main()
