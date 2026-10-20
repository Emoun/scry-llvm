# scry-llvm

An LLVM/Clang backend for the [Scry ISA](https://github.com/Scry-arch/specification),
a register-less architecture that moves data between instructions with
forward-temporal references.  The goal of this first version is
*correctness*: C programs compiled with `clang --target=scry32-unknown-none-elf`
run on the [scryer](https://github.com/Scry-arch/scryer) simulator.
Performance of the compiler and of the generated code is not a goal yet.

What is here:

* `llvm/` – an overlay for llvm-project 22.1.8 containing the `Scry` target
  (`llvm/lib/Target/Scry`), the clang target description and ABI, and an
  lld port for Scry ELF objects.  `patches/` holds the few edits to upstream
  files (triple and ELF registration, driver hooks).
* `runtime/` – `crt0`, a small libc/libm and the list of compiler-rt
  builtins (soft float, signed division) that are compiled for Scry.
* `scripts/` – Python scripts that work on Windows, macOS and Ubuntu to fetch
  LLVM, build the toolchain, build the runtime, build scryer and run tests.
* `tests/c/` – C programs with expected exit codes, executed on scryer.
* `embench/` – board support to run [Embench-IoT](https://github.com/embench/embench-iot).
* `docs/design.md` – how the backend works and the C ABI.

## Prerequisites

| Tool | Ubuntu 22.04/24.04 | macOS | Windows 10/11 |
|------|--------------------|-------|---------------|
| Python 3.8+ | `apt install python3` | included / `brew install python` | python.org installer (add to PATH) |
| CMake 3.20+ and Ninja | `apt install cmake ninja-build` | `brew install cmake ninja` | `choco install cmake ninja` or Visual Studio installer |
| C++17 compiler | `apt install clang lld` (or g++) | Xcode command line tools | Visual Studio 2022 "Desktop development with C++" |
| Rust (for scryer) | [rustup.rs](https://rustup.rs) | [rustup.rs](https://rustup.rs) | [rustup.rs](https://rustup.rs) |
| git | `apt install git` | included | [git-scm.com](https://git-scm.com) |

Building LLVM needs about 20 GB of disk and takes from 20 minutes (16-core
machine) to a few hours (4 cores).  On Windows run the commands below from a
"x64 Native Tools Command Prompt for VS 2022" so that CMake finds MSVC.

## Building

```sh
git clone https://github.com/emoun/scry-llvm.git
cd scry-llvm

# 1. Download llvm-project 22.1.8 into third_party/, copy the overlay and
#    apply the patches.
python3 scripts/setup_llvm.py

# 2. Build clang, lld and the LLVM tools with the Scry backend
#    (installs into ./install; use --prefix to change, --jobs N for parallelism).
python3 scripts/build_llvm.py

# 3. Build crt0, libc, libm and the compiler-rt builtins for Scry and install
#    them next to clang.  Repeat this after every rebuild of clang: the
#    runtime is Scry code, and the test and benchmark scripts warn when it is
#    older than the compiler.
python3 scripts/build_runtime.py --clang install/bin/clang

# 4. Build the scryer simulator (needs cargo); result: build/scryer/release/scryer
python3 scripts/build_scryer.py
```

On Windows use `python` instead of `python3` and `install\bin\clang.exe`.

## Using the compiler

```sh
install/bin/clang --target=scry32-unknown-none-elf -O2 hello.c -o hello.elf
build/scryer/release/scryer hello.elf --target=scry32-unknown-none-elf --machine-mode
echo $?        # exit code = low byte of main's return value
```

* The full triple `scry32-unknown-none-elf` must be given; the driver uses
  it to locate the runtime under `<prefix>/lib/clang-runtimes/<triple>`.
* The driver links with `--gc-sections` by default and the runtime is
  built with one section per function, so an image only carries the
  runtime functions it references (an empty `main` is 40 bytes of code);
  `-Wl,--no-gc-sections` keeps everything.
* `-S` emits assembly in the syntax of the reference `scry_asm` assembler,
  with comments showing the original virtual instructions
  (`clang --target=scry32-unknown-none-elf -S -O2 hello.c`).
* Without `--machine-mode` scryer prints the returned operands and
  simulation metrics (instruction counts) instead of using them as the exit
  code.
* There is no I/O in the simulator: `printf` writes to the in-memory buffer
  `__scry_console`; `exit()` traps (returning from `main` is the normal way
  to finish).  See `docs/design.md` for the C ABI and the other current
  limitations (no VLAs/`alloca`, no `setjmp`, no inline assembly).

## Testing

```sh
# Backend unit tests (lit, FileCheck) for the LLVM target:
python3 scripts/run_lit.py

# C programs run on the simulator at -O0 and -O2 (exit code must match
# the "expect-exit" annotation in each file):
python3 scripts/run_tests.py

# Embench-IoT (cloned into third_party/embench-iot on first use):
python3 scripts/run_embench.py             # all benchmarks
python3 scripts/run_embench.py crc32 edn   # a selection
python3 scripts/run_embench.py --metrics   # also print code metrics (see below)
```

`scripts/run_tests.py` and `scripts/run_embench.py` accept `--clang` and
`--scryer` to point at other binaries.

### Code metrics

`tools/scryer` is a small fork of the scryer driver (`scryer-metrics`, built
by `scripts/build_scryer.py` into `build/scryer-metrics`) that reports how
the generated code behaves: executed instructions, how many of them are
dataflow instructions (`echo`, `dup`, `nop`), per-opcode counts, how far
ahead jumps, calls and returns are issued before they trigger, how far ahead
loads deliver their result, and how many operands are in flight.  It counts
the whole run and the window between the `start_trigger` and `stop_trigger`
symbols (the Embench benchmark body).

```sh
python3 scripts/run_embench.py --metrics --json new.json
python3 scripts/compare_metrics.py docs/perf-results.json new.json
```

Slot loads (`ld.s`) are built right in front of their consumer by design
and are left out of the load distance.  Two static views complement the
simulator counts: `scripts/df_roles.py <source.c or .s>...` breaks the
dataflow instructions of the generated assembly down by role (landing
pads and how many of them are `nop`s, deliveries to successors, jump
conditions, call arguments, hops and ordering fixups), and `clang -mllvm
-stats` prints the lowering's own counters (near and far jumps, reach
hops, ordering fixups, captures, dups, elided casts, slot traffic) for a
compilation.  `scryer-metrics --profile p.json` (or `run_embench.py
--metrics --profile`) writes the execution count of every instruction
address in the window, which `df_roles.py --profile p.json --elf image
sources...` uses to weight the roles by how often each instruction ran
and to report the share executed in the runtime library.
`scripts/rv32_metrics.py` counts the same window on rv32imac images under
Spike, in instructions and in bytes (compressed instructions are two).

`docs/perf-results.md` records the numbers of the current backend per
benchmark, next to those of the correctness-first baseline
(`docs/perf-baseline.md`) and of rv32imac: on the Embench window the
current code executes 0.26 of the baseline's instructions (0.18 of its
non-dataflow instructions) and 2.5 times the rv32imac count (1.5 times
for non-dataflow instructions).

### Status

With llvm-project 22.1.8 and scryer revision `6fc6ac9d`:

* the four lit tests and the 15 C programs in `tests/c/` (at `-O0` and
  `-O2`) pass;
* 17 of the 19 Embench-IoT benchmarks build at `-O2` and verify on the
  simulator (aha-mont64, crc32, depthconv, edn, matmult-int, md5sum,
  nettle-aes, nettle-sha256, nsichneu, picojpeg, qrduino, sglib-combined,
  slre, statemate, tarfind, ud, xgboost);
* `huffbench` and `wikisort` need stack frames of 2 KB and 4 KB and cannot
  run on the simulator, whose stack is a fixed 4 KB buffer; the runner
  reports them as `XFAIL` (pass `--strict` to count them as failures).

Each benchmark takes between 15 s and 70 s to simulate, so a full run takes
about 10 minutes.

## Developing the backend

Run `scripts/setup_llvm.py --link` to symlink the overlay into the LLVM tree
so that edits in `llvm/` are picked up by `ninja -C build/llvm clang`
directly.  After changing an upstream file inside `third_party/llvm-project`,
regenerate `patches/` with `scripts/make_patches.py` (the LLVM tree must have
a git baseline commit of the pristine sources: run `setup_llvm.py --git-baseline`
before making changes).

The continuous-integration workflow in `.github/workflows/ci.yml` performs
all of the above on Ubuntu, macOS and Windows.

## License

The LLVM overlay and patches are licensed under the Apache License v2.0 with
LLVM Exceptions, like LLVM itself; everything else in this repository is
licensed under the Apache License v2.0 (see `LICENSE`).
