#!/usr/bin/env python3
"""Configure, build and install LLVM/Clang/lld with the Scry backend.

Usage:
    build_llvm.py [--llvm-src <dir>] [--build-dir <dir>] [--prefix <dir>]
                  [--jobs N] [--build-type Release|Debug|RelWithDebInfo]
                  [--no-assertions] [--configure-only] [--extra <cmake arg> ...]

Requires CMake >= 3.20, Ninja, and a C++17 host compiler (clang, gcc or MSVC).
On Windows run from a "x64 Native Tools" prompt or let CMake find MSVC.
"""
import argparse
import multiprocessing
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

TOOLS = ["clang", "lld", "llc", "llvm-mc", "llvm-ar", "llvm-nm", "llvm-objdump",
         "llvm-readelf", "llvm-readobj", "llvm-size", "llvm-strip", "llvm-objcopy",
         "llvm-link", "opt", "llvm-dis", "llvm-as", "FileCheck", "count", "not"]


def run(cmd, cwd=None):
    print("  " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, cwd=cwd)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--llvm-src", default=str(ROOT / "third_party" / "llvm-project"))
    ap.add_argument("--build-dir", default=str(ROOT / "build" / "llvm"))
    ap.add_argument("--prefix", default=str(ROOT / "install"))
    ap.add_argument("--jobs", type=int, default=multiprocessing.cpu_count())
    ap.add_argument("--build-type", default="Release")
    ap.add_argument("--no-assertions", action="store_true")
    ap.add_argument("--configure-only", action="store_true")
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--extra", nargs="*", default=[], help="extra cmake arguments")
    args = ap.parse_args()

    src = pathlib.Path(args.llvm_src).resolve() / "llvm"
    build = pathlib.Path(args.build_dir).resolve()
    prefix = pathlib.Path(args.prefix).resolve()
    build.mkdir(parents=True, exist_ok=True)

    cmake = [
        "cmake", "-G", "Ninja", "-S", str(src), "-B", str(build),
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DLLVM_ENABLE_ASSERTIONS=" + ("OFF" if args.no_assertions else "ON"),
        "-DLLVM_ENABLE_PROJECTS=clang;lld",
        "-DLLVM_TARGETS_TO_BUILD=",
        "-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Scry",
        "-DLLVM_DEFAULT_TARGET_TRIPLE=scry32-unknown-none-elf",
        "-DLLVM_INCLUDE_TESTS=ON",
        "-DCLANG_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_EXAMPLES=OFF",
        "-DLLVM_INCLUDE_BENCHMARKS=OFF",
        "-DLLVM_INCLUDE_DOCS=OFF",
        "-DLLVM_ENABLE_BINDINGS=OFF",
        "-DLLVM_ENABLE_ZSTD=OFF",
        "-DLLVM_ENABLE_LIBXML2=OFF",
        "-DLLVM_ENABLE_LIBEDIT=OFF",
        "-DLLVM_ENABLE_LIBPFM=OFF",
        "-DCLANG_ENABLE_STATIC_ANALYZER=OFF",
        "-DCLANG_ENABLE_ARCMT=OFF",
        "-DLLVM_PARALLEL_LINK_JOBS=2",
        "-DLLVM_INSTALL_TOOLCHAIN_ONLY=OFF",
        "-DLLVM_TOOLCHAIN_TOOLS=" + ";".join(TOOLS),
    ]
    if os.name != "nt":
        if shutil.which("clang") and shutil.which("clang++"):
            cmake += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
        if shutil.which("ld.lld"):
            cmake += ["-DLLVM_USE_LINKER=lld"]
    cmake += args.extra
    run(cmake)
    if args.configure_only:
        return
    run(["ninja", "-C", str(build), f"-j{args.jobs}", *TOOLS])
    if not args.no_install:
        run(["ninja", "-C", str(build), "install-distribution"] if False else
            ["cmake", "--install", str(build), "--prefix", str(prefix)])
    print("Done. Toolchain in", prefix / "bin")


if __name__ == "__main__":
    main()
