# Performance results

Executed-instruction metrics of the current backend on the 19 Embench-IoT
benchmarks (huffbench and wikisort since the simulator's stack became
configurable; the scripts give it 256 KiB), measured like the baseline
(`perf-baseline.md`): from the simulator's event stream (`scripts/scry_metrics.py`) over the benchmark window
between `start_trigger` and `stop_trigger`, runtime library included.
The raw data is `perf-results.json`; `scripts/compare_metrics.py
docs/perf-results.json new.json` compares a new build against it.  A
second table below gives the executed instruction bytes and the static
code size against rv32imac.  `perf-metrics.md` breaks the same run down
by instruction class and lifetime.

Columns: baseline and current executed instructions and their ratio;
current non-dataflow instructions (everything but `echo`, `dup` and
`nop`) and their ratio to the baseline's; share of dataflow instructions;
mean distance (instructions) between a `jmp` and its trigger; mean `ld`
output reference (slot loads, built in front of their consumer, are
left out); mean and maximum number of operands in flight in the
current frame (operands produced and not yet consumed, from the
simulator's operand events); instructions executed by the same window on rv32imac
(Spike; the same clang 22.1.8 at -O2 with this repository's libc and
compiler-rt builtins, built by `scripts/build_rv32.py`) and the ratios of
the current total and non-dataflow counts to it.

| benchmark | baseline | current | ratio | non-DF | ratio | DF | jmp dist | ld ref | in flight | rv32imac | /rv32 | non-DF/rv32 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 24,356,988 | 5,324,355 | 0.22 | 3,428,300 | 0.17 | 36% | 40 | 11.1 | 4.0 / 16 | 6,234,278 | 0.9 | 0.5 |
| crc32 | 24,912,394 | 8,016,581 | 0.32 | 6,272,369 | 0.29 | 22% | 14 | 3.0 | 1.3 / 3 | 3,832,176 | 2.1 | 1.6 |
| depthconv | 19,961,445 | 8,144,232 | 0.41 | 4,777,715 | 0.27 | 41% | 487 | 22.2 | 53.7 / 78 | 2,730,597 | 3.0 | 1.7 |
| edn | 36,452,182 | 9,842,737 | 0.27 | 6,156,418 | 0.20 | 37% | 57 | 13.3 | 12.1 / 97 | 3,561,390 | 2.8 | 1.7 |
| huffbench | - | 10,755,154 | - | 6,402,936 | - | 40% | 7 | 2.3 | 3.4 / 31 | 2,494,095 | 4.3 | 2.6 |
| matmult-int | 22,864,569 | 3,935,333 | 0.17 | 2,160,901 | 0.11 | 45% | 378 | 16.2 | 73.6 / 113 | 1,264,277 | 3.1 | 1.7 |
| md5sum | 26,396,891 | 9,551,541 | 0.36 | 4,707,594 | 0.21 | 51% | 20 | 16.9 | 7.2 / 18 | 2,577,940 | 3.7 | 1.8 |
| nettle-aes | 31,484,596 | 8,349,155 | 0.27 | 4,120,582 | 0.16 | 51% | 126 | 23.1 | 15.8 / 27 | 3,512,553 | 2.4 | 1.2 |
| nettle-sha256 | 69,905,081 | 10,836,207 | 0.16 | 6,426,712 | 0.11 | 41% | 327 | 15.4 | 30.3 / 54 | 5,213,716 | 2.1 | 1.2 |
| nsichneu | 17,034,943 | 6,140,403 | 0.36 | 5,510,842 | 0.37 | 10% | 0 | 0.5 | 0.2 / 3 | 2,008,184 | 3.1 | 2.7 |
| picojpeg | 35,334,763 | 8,204,717 | 0.23 | 5,039,423 | 0.17 | 39% | 44 | 5.8 | 12.7 / 33 | 2,695,985 | 3.0 | 1.9 |
| qrduino | 35,476,913 | 7,658,528 | 0.22 | 4,425,624 | 0.15 | 42% | 6 | 3.9 | 4.9 / 66 | 2,879,462 | 2.7 | 1.5 |
| sglib-combined | 40,141,033 | 10,335,282 | 0.26 | 6,750,650 | 0.19 | 35% | 5 | 1.1 | 1.8 / 31 | 2,661,222 | 3.9 | 2.5 |
| slre | 22,370,915 | 9,110,952 | 0.41 | 6,503,607 | 0.33 | 29% | 2 | 1.5 | 0.4 / 7 | 2,903,283 | 3.1 | 2.2 |
| statemate | 25,527,859 | 9,853,681 | 0.39 | 7,962,232 | 0.36 | 19% | 26 | 3.8 | 8.6 / 33 | 1,948,089 | 5.1 | 4.1 |
| tarfind | 25,816,534 | 3,484,245 | 0.13 | 2,343,717 | 0.11 | 33% | 15 | 2.5 | 2.6 / 18 | 1,208,288 | 2.9 | 1.9 |
| ud | 64,761,665 | 7,022,696 | 0.11 | 4,027,457 | 0.07 | 43% | 17 | 6.5 | 4.2 / 16 | 2,643,648 | 2.7 | 1.5 |
| wikisort | - | 5,142,692 | - | 3,351,313 | - | 35% | 6 | 1.1 | 3.0 / 11 | 1,721,254 | 3.0 | 1.9 |
| xgboost | 32,029,599 | 8,713,233 | 0.27 | 4,149,881 | 0.15 | 52% | 7 | 2.8 | 4.0 / 19 | 3,387,528 | 2.6 | 1.2 |
| **total, 17 recorded** | 554,828,370 | 134,523,878 | 0.24 | 84,764,024 | 0.18 | | | | | | | |
| **total** | - | 150,421,724 | - | 94,518,273 | - | 37% | | | | 55,477,965 | 2.71 | 1.70 |

Geometric means of the per-benchmark ratios: current/baseline 0.250
(non-dataflow 0.181); current/rv32imac 2.81
(non-dataflow 1.75).

An earlier version of this comparison compiled the rv32imac images with
GCC (riscv64-unknown-elf-gcc -O2, libgcc): 61,776,643 instructions and
196,731,492 bytes on the window and 206,044 bytes of code, i.e. ratios
of 2.43, 1.53 and 1.08.  Those images were 2.7 times the size of the
clang-built ones, so the code-size comparison in particular has changed;
the figures here supersede them.

### Instruction bytes and static code

A Scry instruction is two bytes; rv32imac mixes two-byte compressed and
four-byte instructions (counted from Spike's instruction log by
`scripts/rv32_metrics.py`: an instruction is compressed when the low two
bits of its encoding are not 11).  Columns: executed bytes on the window
for Scry and rv32imac and their ratio; the compressed share of the
rv32imac instructions; bytes of executable code in the linked image
(benchmark, Embench support files and the referenced runtime, both
toolchains linking with function sections and `--gc-sections`) and
their ratio.

| benchmark | Scry bytes | rv32imac bytes | ratio | rv32 c16 | Scry text | rv32imac text | ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 10,648,710 | 20,001,766 | 0.53 | 40% | 3,116 | 2,364 | 1.32 |
| crc32 | 16,033,162 | 10,799,840 | 1.48 | 59% | 480 | 324 | 1.48 |
| depthconv | 16,288,464 | 9,270,234 | 1.76 | 30% | 2,158 | 506 | 4.26 |
| edn | 19,685,474 | 11,744,992 | 1.68 | 35% | 8,456 | 1,748 | 4.84 |
| huffbench | 21,510,308 | 7,062,252 | 3.05 | 58% | 8,814 | 2,300 | 3.83 |
| matmult-int | 7,870,666 | 3,949,258 | 1.99 | 44% | 7,726 | 1,288 | 6.00 |
| md5sum | 19,103,082 | 6,911,360 | 2.76 | 66% | 5,314 | 1,350 | 3.94 |
| nettle-aes | 16,698,310 | 11,813,894 | 1.41 | 32% | 6,998 | 2,972 | 2.35 |
| nettle-sha256 | 21,672,414 | 15,845,124 | 1.37 | 48% | 12,158 | 6,304 | 1.93 |
| nsichneu | 12,280,806 | 6,379,356 | 1.93 | 41% | 21,450 | 11,142 | 1.93 |
| picojpeg | 16,409,434 | 8,747,590 | 1.88 | 38% | 55,448 | 16,742 | 3.31 |
| qrduino | 15,317,056 | 9,208,914 | 1.66 | 40% | 33,622 | 10,216 | 3.29 |
| sglib-combined | 20,670,564 | 7,363,012 | 2.81 | 62% | 12,286 | 3,142 | 3.91 |
| slre | 18,221,904 | 8,363,678 | 2.18 | 56% | 9,312 | 3,266 | 2.85 |
| statemate | 19,707,362 | 6,873,206 | 2.87 | 24% | 10,630 | 3,724 | 2.85 |
| tarfind | 6,968,490 | 3,748,936 | 1.86 | 45% | 2,918 | 1,062 | 2.75 |
| ud | 14,045,392 | 7,811,304 | 1.80 | 52% | 4,268 | 1,304 | 3.27 |
| wikisort | 10,285,384 | 4,998,950 | 2.06 | 55% | 16,334 | 6,966 | 2.34 |
| xgboost | 17,426,466 | 10,920,064 | 1.60 | 39% | 1,036 | 752 | 1.38 |
| **total** | 300,843,448 | 171,813,730 | 1.75 | 45% | 222,524 | 77,472 | 2.87 |

How the count was reduced, in the order the work was done (each step's
own figure is in its commit message, measured on the window with the
runtime library of the time):

1. jumps issued early, near and far forms, a branch polarity operand;
2. single-block values kept in flight instead of in frame slots, with
   dups, hops and ordering fixups by the lowering; constants built in
   front of their consumer; compares delivered straight to their jump;
3. values live across blocks carried in flight over the edges (landing
   pads, position colouring, a cost model against slots);
4. a scheduling strategy issuing branch conditions and loads first;
5. constants and addresses hoisted out of loops by machine LICM and
   rebuilt, slotted or carried in flight by cost; PC-relative symbol
   chains of three instructions; indexed loads and stores; the frame
   ordered by hotness; slot values loaded once per block;
6. the emitter hops exactly the outputs that would expire (iterating to
   a fixed point) instead of one per pending output, which removed most
   `dup; echo; echo` triples (-8.3% instructions, a third of the echoes);
   a doubled inlining threshold (-2.2%); word-wise copy and fill routines
   in the runtime (-3.8%; tarfind -33%, statemate -10%); zero extensions of values in flight dropped
   where the consumer takes the wider type anyway (-0.7%); and a
   scheduler rule keeping the consumers of a load's result behind other
   ready work (mean `ld` reference 6.1 to 6.4 for +0.3% dataflow
   instructions).  Each figure is against the same build with that one
   change off;
7. (round 3) small innermost loops unrolled by four (-3.2%; aha-mont64
   -18%, edn -12%, matmult-int -9%; static code +29%); the copy and
   fill routines working eight bytes at a time with unrolled loops
   (-1.8%; tarfind -26%, matmult-int -12%, md5sum -5%); signed compares building their constant with the signed
   type instead of casting it, and right shifts by one as one-input
   divisions (-0.6%; nsichneu -5%); the runtime compiled at -O2 with one
   section per function and the driver linking with `--gc-sections`
   (-0.3% executed; every image 17 KB of code smaller).

Measured in the same way and not kept: tail-duplicating small branch
blocks (LLVM's own size limit and a dedicated pass in SSA form; +0.0 to
0.1% instructions, mean jump distance +0.1: the jumps that trigger at
themselves sit in blocks that only carry values on, not in blocks that
compute a condition); colouring landing positions by need (1% fewer
`nop`s but +0.02% instructions, the freed positions costing hops
elsewhere); scheduling the last in-block use of a delivered value late
(+0.06%, distances unchanged); telling loop strength reduction that the
indexed addressing form `base + index * size` is free (total unchanged,
non-dataflow instructions +1.6%: matmult-int +7% with its loads losing
their distance, md5sum -7%).  Global-pointer-relative addressing was
estimated rather than built: with the image above 64 KiB a symbol
address costs three instructions, and `ld [gp, idx]` would need one or
two for the index plus a `dup` to keep the pointer in flight per use,
so it converts about one non-dataflow instruction per global access
into a dataflow one without reducing the count.

Round 3 was measured with execution profiles (`scry_metrics.py
--profile`, attributed to the assembly with `scripts/df_roles.py`),
which settled three more candidates without building them: far-jump
target chains are 0.7% of the executed instructions, the upper bound of
a layout that keeps every jump in the near form; the jumps that trigger
at themselves (34% of jumps) are the volatile compare-and-jump blocks of
nsichneu's state machine and the pointer chasing of sglib and slre,
where if-conversion has nothing to convert; and the doubled inlining
threshold costs only 1.4% of static code (ud +25%), so no size-aware
variant is needed.  Symbol address chains are 2.5% of the executed
instructions (crc32 and statemate 13% each), the share a two-instruction
address form would attack; the small-address code model that would give
one (image base just above the simulator's stack, heap last) was
considered and not adopted.  The ISA's compares convert a mixed
signed/unsigned pair to unsigned in the simulator, so a signed compare
cannot leave a known non-negative operand untagged.

With scryer 01216f0 zero-initialised globals moved from `.data` to
`.bss`, which changed no code (static bytes identical) but shifted the
data layout: in nettle-sha256 the message buffer landed at an odd
address and `memcpy` fell back from eight-byte words to bytes, +157,360
executed instructions (+1.5%; everything else identical).  Aligning
byte arrays of eight bytes or more to eight, as other targets do for
large arrays, would remove this sensitivity and is a candidate for a
later round.

What remains is largely inherent to the instruction set as the simulator
implements it: an immediate costs a `const` (plus a `grow` per further
byte), a symbol address three instructions, a shift by a constant two,
a narrow load that is widened a `cast` where the width matters; and
every value used more than once or further than 31 instructions away
costs a `dup` or an `echo`.  Of the remaining dataflow instructions
(static, over the Embench sources) landing pads are a third (two
positions in five a `nop`), copies, hops and ordering fixups inside
expansions about 58%, deliveries to successors 6%
(`scripts/df_roles.py`).
