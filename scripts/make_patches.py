#!/usr/bin/env python3
"""Regenerate patches/llvm-<version>/*.patch from a modified llvm-project
checkout that has a git baseline commit of the pristine sources.

The overlay files (everything under llvm/ in this repository) are excluded;
only modifications to upstream files are captured.

Usage:
    make_patches.py [--llvm-src <dir>]
"""
import argparse
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
LLVM_VERSION = "22.1.8"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--llvm-src", default=str(ROOT / "third_party" / "llvm-project"))
    args = ap.parse_args()
    llvm = pathlib.Path(args.llvm_src).resolve()
    overlay = ROOT / "llvm"
    overlay_files = {str(p.relative_to(overlay)) for p in overlay.rglob("*") if p.is_file()}

    diff = subprocess.run(["git", "-C", str(llvm), "diff", "--no-color"],
                          check=True, capture_output=True, text=True).stdout
    # Split per file and drop overlay files (they are symlinks or copies).
    chunks = diff.split("diff --git ")
    out = []
    for chunk in chunks:
        if not chunk.strip():
            continue
        header = chunk.splitlines()[0]
        path = header.split(" b/")[-1]
        if path in overlay_files:
            continue
        out.append("diff --git " + chunk)
    dest = ROOT / "patches" / f"llvm-{LLVM_VERSION}"
    dest.mkdir(parents=True, exist_ok=True)
    patch = dest / "0001-scry-target-registration.patch"
    patch.write_text("".join(out))
    print(f"Wrote {patch} ({len(out)} files)")


if __name__ == "__main__":
    main()
