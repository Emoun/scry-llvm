#!/usr/bin/env python3
"""Build the Embench-IoT benchmarks for rv32imac with clang and, with
--spike, count their instructions under Spike (scripts/rv32_metrics.py).

The images are the comparison point for the Scry figures of
run_embench.py and are built from the same sources with the same compiler:
the benchmark, the Embench support files, the board support in
embench/config/rv32 (crt0.S reports main's result through Spike's HTIF
tohost register), this repository's libc and libm (runtime/) and the
compiler-rt builtins of runtime/builtins.txt, all compiled with

    clang --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -O2
          -ffreestanding -nostdlibinc -ffunction-sections -fdata-sections

and linked with lld, --gc-sections and --relax-gp (global-pointer
relaxation, as the GNU linker does by default).  Any clang with the RISC-V
target works; for the recorded figures it is the clang of this repository
built with `build_llvm.py --targets RISCV`, so that both ISAs use the same
LLVM version.  Images go to build/rv32/<name>.elf.

Usage:
    build_rv32.py [--clang <clang>] [--llvm-src <dir>] [--embench <dir>]
                  [--spike <spike>] [--json out.json] [-O 2] [benchmarks...]
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRIPLE = "riscv32-unknown-elf"
ARCH = ["-march=rv32imac", "-mabi=ilp32"]
EXE = ".exe" if os.name == "nt" else ""
EMBENCH_REPO = "https://github.com/embench/embench-iot.git"
EMBENCH_REV = "09c2ed8c3b7008c95d08b038de4a3f6dc103ed70"


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit("command failed: " + " ".join(str(c) for c in cmd) + "\n" + r.stderr[-3000:])
    return r


def tool(clang, name):
    """An LLVM tool next to clang, with clang's version suffix (clang-18 ->
    llvm-ar-18) when the plain name is not there, else from PATH."""
    cands = [clang.parent / (name + EXE)]
    m = re.search(r"(-\d+)$", clang.stem)
    if m:
        cands.append(clang.parent / (name + m.group(1) + EXE))
    for c in cands:
        if c.exists():
            return c
    return pathlib.Path(name)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang", default=str(ROOT / "install" / "bin" / ("clang" + EXE)),
                    help="clang with the RISC-V target")
    ap.add_argument("--llvm-src", default=str(ROOT / "third_party" / "llvm-project"),
                    help="llvm-project source tree (for the compiler-rt builtins)")
    ap.add_argument("--embench", default=str(ROOT / "third_party" / "embench-iot"))
    ap.add_argument("--out", default=str(ROOT / "build" / "rv32"))
    ap.add_argument("-O", "--opt", default="2")
    ap.add_argument("--gsf", default="1", help="global scale factor")
    ap.add_argument("--warmup", default="1", help="warm-up heat")
    ap.add_argument("--cflags", action="append", default=[],
                    help="extra compiler flag (repeatable; use --cflags=-x)")
    ap.add_argument("--spike", help="run scripts/rv32_metrics.py with this Spike on the images")
    ap.add_argument("--json", help="with --spike: write the metrics to this JSON file")
    ap.add_argument("benchmarks", nargs="*")
    args = ap.parse_args()

    clang = pathlib.Path(args.clang)
    if not clang.exists():
        sys.exit(f"clang not found at {clang}; pass --clang")
    ar = tool(clang, "llvm-ar")
    lld = tool(clang, "ld.lld")
    embench = pathlib.Path(args.embench)
    if not (embench / "src").exists():
        embench.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", EMBENCH_REPO, str(embench)])
        run(["git", "-C", str(embench), "checkout", EMBENCH_REV])
    config = ROOT / "embench" / "config" / "rv32"
    support = embench / "support"
    out = pathlib.Path(args.out)
    rt = out / "runtime"
    rt.mkdir(parents=True, exist_ok=True)

    cc = [str(clang), "--target=" + TRIPLE, *ARCH, "-O" + args.opt, "-g0", "-ffreestanding",
          "-nostdlibinc", "-ffunction-sections", "-fdata-sections", *args.cflags]
    inc = ["-I", str(ROOT / "runtime" / "libc" / "include")]

    def compile_one(src, obj, extra):
        run([*cc, *extra, "-c", str(src), "-o", str(obj)])
        return obj

    print("== runtime (crt0, libc, libm, compiler-rt builtins)", flush=True)
    crt0 = compile_one(config / "crt0.S", rt / "crt0.o", [])
    objs = [compile_one(src, rt / (src.stem + ".o"), inc) for src in
            sorted((ROOT / "runtime" / "libc" / "src").glob("*.c"))
            + sorted((ROOT / "runtime" / "libm").glob("*.c"))]
    libc = rt / "libc.a"
    if libc.exists():
        libc.unlink()
    run([str(ar), "rcs", str(libc), *map(str, objs)])
    builtins_dir = pathlib.Path(args.llvm_src) / "compiler-rt" / "lib" / "builtins"
    if not builtins_dir.exists():
        sys.exit(f"compiler-rt builtins not found under {builtins_dir}; run setup_llvm.py or pass --llvm-src")
    objs = []
    for line in (ROOT / "runtime" / "builtins.txt").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        objs.append(compile_one(builtins_dir / line, rt / ("rt_" + pathlib.Path(line).stem + ".o"),
                                ["-I", str(builtins_dir), "-D_YUGA_LITTLE_ENDIAN=1",
                                 "-D_YUGA_BIG_ENDIAN=0", "-Wno-unknown-pragmas"]))
    librt = rt / "libclang_rt.builtins.a"
    if librt.exists():
        librt.unlink()
    run([str(ar), "rcs", str(librt), *map(str, objs)])

    names = args.benchmarks or sorted(p.name for p in (embench / "src").iterdir() if p.is_dir())
    images = []
    print(f"{'benchmark':18} {'elf':>8} {'text':>8}")
    for name in names:
        srcdir = embench / "src" / name
        sources = sorted(srcdir.glob("*.c")) + [support / "main.c", support / "beebsc.c",
                                                config / "boardsupport.c"]
        elf = out / (name + ".elf")
        run([*cc, "-I", str(support), "-I", str(config), "-I", str(srcdir), *inc,
             f"-DWARMUP_HEAT={args.warmup}", f"-DGLOBAL_SCALE_FACTOR={args.gsf}",
             "-DHAVE_BOARDSUPPORT_H", *[str(s) for s in sources], str(crt0),
             str(libc), str(librt), "-nostdlib", f"--ld-path={lld}",
             "-T", str(config / "link.ld"), "-Wl,--gc-sections", "-Wl,--relax-gp",
             "-o", str(elf)])
        images.append(elf)
        print(f"{name:18} {elf.stat().st_size:8d} {text_size(elf):8d}", flush=True)

    if args.spike:
        cmd = [sys.executable, str(ROOT / "scripts" / "rv32_metrics.py"), "--spike", args.spike,
               "--nm", str(tool(clang, "llvm-nm")), "--objdump", str(tool(clang, "llvm-objdump"))]
        if args.json:
            cmd += ["--json", args.json]
        sys.exit(subprocess.run(cmd + [str(i) for i in images]).returncode)


def text_size(elf):
    """Bytes of the executable sections of a 32-bit little-endian ELF file."""
    import struct
    with open(elf, "rb") as f:
        data = f.read()
    shoff, shentsize, shnum = struct.unpack_from("<I", data, 32)[0], *struct.unpack_from("<HH", data, 46)
    total = 0
    for i in range(shnum):
        sh_type, sh_flags, _, _, sh_size = struct.unpack_from("<IIIII", data, shoff + i * shentsize + 4)
        if sh_type == 1 and sh_flags & 0x4:
            total += sh_size
    return total


if __name__ == "__main__":
    main()
