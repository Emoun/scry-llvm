#!/usr/bin/env python3
"""Build the Scry runtime (crt0, libc, libm, compiler-rt builtins) with the
Scry-enabled clang and install it where the clang driver looks for it:

    <prefix>/lib/clang-runtimes/scry32-unknown-none-elf/lib/{crt0.o,libc.a,libm.a}
    <prefix>/lib/clang-runtimes/scry32-unknown-none-elf/include/*.h
    <clang resource dir>/lib/scry32-unknown-none-elf/libclang_rt.builtins.a

Usage:
    build_runtime.py --clang <path/to/clang> [--llvm-ar <path>] [--prefix <dir>]
                     [--llvm-src <llvm-project dir>] [--build-dir <dir>]

Works on Linux, macOS and Windows (only Python and the built toolchain are
needed).
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRIPLE = "scry32-unknown-none-elf"


def run(cmd, **kw):
    print("  " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def compile_one(clang, src, obj, extra):
    # One section per function and per object, so that the linker's
    # --gc-sections (the Scry driver's default) keeps only what a program
    # references: an archive member is a whole source file otherwise.
    run([clang, "--target=" + TRIPLE, "-c", "-O2", "-ffreestanding",
         "-fno-builtin", "-nostdlib", "-ffunction-sections", "-fdata-sections",
         "-Wno-unused-command-line-argument", *extra, "-o", obj, src])


def archive(ar, out, objs):
    if out.exists():
        out.unlink()
    run([ar, "rcs", out, *objs])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang", required=True, help="Scry-enabled clang")
    ap.add_argument("--llvm-ar", help="llvm-ar (default: next to clang)")
    ap.add_argument("--prefix", help="install prefix (default: clang's prefix, i.e. <bin>/..)")
    ap.add_argument("--llvm-src", default=str(ROOT / "third_party" / "llvm-project"),
                    help="llvm-project source tree (for compiler-rt builtins)")
    ap.add_argument("--build-dir", default=str(ROOT / "build" / "runtime"))
    args = ap.parse_args()

    clang = pathlib.Path(args.clang).resolve()
    bindir = clang.parent
    ar = pathlib.Path(args.llvm_ar) if args.llvm_ar else bindir / ("llvm-ar.exe" if os.name == "nt" else "llvm-ar")
    if not ar.exists():
        sys.exit(f"llvm-ar not found at {ar}")
    prefix = pathlib.Path(args.prefix).resolve() if args.prefix else bindir.parent
    build = pathlib.Path(args.build_dir).resolve()
    build.mkdir(parents=True, exist_ok=True)

    resource_dir = pathlib.Path(subprocess.run(
        [str(clang), "--target=" + TRIPLE, "-print-resource-dir"],
        check=True, capture_output=True, text=True).stdout.strip())

    sysroot = prefix / "lib" / "clang-runtimes" / TRIPLE
    libdir = sysroot / "lib"
    incdir = sysroot / "include"
    rtdir = resource_dir / "lib" / TRIPLE
    for d in (libdir, incdir, rtdir):
        d.mkdir(parents=True, exist_ok=True)

    # Headers.
    for h in (ROOT / "runtime" / "libc" / "include").glob("*.h"):
        shutil.copy(h, incdir / h.name)
    inc = ["-I", str(ROOT / "runtime" / "libc" / "include")]

    print("== crt0")
    compile_one(clang, ROOT / "runtime" / "crt" / "crt0.c", build / "crt0.o", inc)
    shutil.copy(build / "crt0.o", libdir / "crt0.o")

    print("== libc")
    objs = []
    for src in sorted((ROOT / "runtime" / "libc" / "src").glob("*.c")) + \
               sorted((ROOT / "runtime" / "libm").glob("*.c")):
        obj = build / (src.stem + ".o")
        compile_one(clang, src, obj, inc)
        objs.append(obj)
    archive(ar, libdir / "libc.a", objs)
    # libm is folded into libc; provide an archive so -lm works.
    archive(ar, libdir / "libm.a", [build / "math.o"])

    print("== compiler-rt builtins")
    builtins_dir = pathlib.Path(args.llvm_src) / "compiler-rt" / "lib" / "builtins"
    objs = []
    for line in (ROOT / "runtime" / "builtins.txt").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        src = builtins_dir / line
        obj = build / ("rt_" + pathlib.Path(line).stem + ".o")
        compile_one(clang, src, obj, ["-I", str(builtins_dir), "-D_YUGA_LITTLE_ENDIAN=1",
                                      "-D_YUGA_BIG_ENDIAN=0", "-Wno-unknown-pragmas"])
        objs.append(obj)
    archive(ar, rtdir / "libclang_rt.builtins.a", objs)

    print(f"Runtime installed under {sysroot} and {rtdir}")


if __name__ == "__main__":
    main()
