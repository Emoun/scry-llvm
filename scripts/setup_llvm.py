#!/usr/bin/env python3
"""Prepare the LLVM source tree for the Scry backend.

1. Download the pinned llvm-project release tarball (or reuse an existing
   checkout given with --llvm-src),
2. copy (or, with --link, symlink) the overlay files from llvm/ into it,
3. apply the patches in patches/ to the upstream files that need small
   modifications (triple registration, ELF constants, driver hooks, ...).

Usage:
    setup_llvm.py [--llvm-src <dir>] [--link]

Needs Python 3.8+ and either the 'patch' tool or git (patches are applied
with "git apply" when git is available, else with Python's own applier).
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
LLVM_VERSION = "22.1.8"
TARBALL = f"llvm-project-{LLVM_VERSION}.src.tar.xz"
URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/{TARBALL}"


def download(dest: pathlib.Path):
    if dest.exists():
        return
    print(f"Downloading {URL} ...", flush=True)
    tmp = dest.with_suffix(".part")
    with urllib.request.urlopen(URL) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    tmp.rename(dest)


def copy_overlay(overlay: pathlib.Path, llvm: pathlib.Path, link: bool):
    for src in overlay.rglob("*"):
        if src.is_dir():
            continue
        rel = src.relative_to(overlay)
        dst = llvm / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.is_symlink() or dst.exists():
            dst.unlink()
        if link:
            os.symlink(src, dst)
        else:
            shutil.copy(src, dst)
    print(f"Overlay {'linked' if link else 'copied'} into {llvm}")


def apply_patches(llvm: pathlib.Path):
    patches = sorted((ROOT / "patches" / f"llvm-{LLVM_VERSION}").glob("*.patch"))
    if not patches:
        print("No patches to apply")
        return
    git = shutil.which("git")
    git_apply = None
    if git:
        # When the LLVM tree lives inside a git checkout (e.g. third_party/ of
        # this repository), git interprets patch paths relative to that
        # repository's root and silently skips everything else.  Prefix the
        # paths with the tree's location in the repository so they resolve.
        git_apply = [git, "apply"]
        prefix = subprocess.run([git, "rev-parse", "--show-prefix"], cwd=llvm,
                                capture_output=True, text=True)
        if prefix.returncode == 0 and prefix.stdout.strip():
            git_apply.append("--directory=" + prefix.stdout.strip().rstrip("/"))
    for orig in patches:
        print(f"Applying {orig.name}")
        # A checkout with CRLF conversion (Git for Windows' default) would
        # make the patch mismatch the LF upstream sources: apply an LF copy.
        p = llvm.parent / (orig.name + ".lf")
        p.write_bytes(orig.read_bytes().replace(b"\r\n", b"\n"))
        try:
            if git_apply:
                # --check first so re-running setup is harmless.
                chk = subprocess.run(git_apply + ["--check", "--reverse", str(p)],
                                     cwd=llvm, capture_output=True)
                if chk.returncode == 0:
                    print("  already applied")
                    continue
                subprocess.run(git_apply + ["--check", str(p)], cwd=llvm, check=True)
                subprocess.run(git_apply + [str(p)], cwd=llvm, check=True)
            else:
                subprocess.run(["patch", "-p1", "-N", "-i", str(p)], cwd=llvm, check=True)
        finally:
            p.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--llvm-src", default=str(ROOT / "third_party" / "llvm-project"))
    ap.add_argument("--link", action="store_true",
                    help="symlink overlay files instead of copying (for development)")
    ap.add_argument("--skip-download", action="store_true")
    ap.add_argument("--git-baseline", action="store_true",
                    help="record the pristine sources in a git commit inside the LLVM tree "
                         "(needed by scripts/make_patches.py)")
    args = ap.parse_args()

    llvm = pathlib.Path(args.llvm_src).resolve()
    if not (llvm / "llvm" / "CMakeLists.txt").exists():
        if args.skip_download:
            sys.exit(f"{llvm} does not contain llvm-project")
        llvm.parent.mkdir(parents=True, exist_ok=True)
        tarball = llvm.parent / TARBALL
        download(tarball)
        print("Extracting (this takes a while) ...", flush=True)
        with tarfile.open(tarball) as tf:
            if hasattr(tarfile, "data_filter"):
                tf.extractall(llvm.parent, filter="data")
            else:
                tf.extractall(llvm.parent)
        (llvm.parent / f"llvm-project-{LLVM_VERSION}.src").rename(llvm)
        tarball.unlink()

    if args.git_baseline and not (llvm / ".git").exists():
        git = shutil.which("git")
        if not git:
            sys.exit("--git-baseline needs git")
        print("Recording pristine baseline commit (this takes a minute) ...", flush=True)
        subprocess.run([git, "init", "-q"], cwd=llvm, check=True)
        subprocess.run([git, "add", "-A"], cwd=llvm, check=True)
        subprocess.run([git, "-c", "user.name=scry", "-c", "user.email=scry@localhost",
                        "commit", "-q", "-m", f"pristine llvm-project {LLVM_VERSION}"],
                       cwd=llvm, check=True)

    copy_overlay(ROOT / "llvm", llvm, args.link)
    apply_patches(llvm)
    print("LLVM source tree is ready:", llvm)


if __name__ == "__main__":
    main()
