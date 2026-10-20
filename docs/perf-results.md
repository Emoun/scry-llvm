# Performance results

Executed-instruction metrics of the current backend on the 17 Embench-IoT
benchmarks that run on scryer, measured like the baseline
(`perf-baseline.md`): with `scryer-metrics` over the benchmark window
between `start_trigger` and `stop_trigger`, runtime library included.
The raw data is `perf-results.json`; `scripts/compare_metrics.py
docs/perf-results.json new.json` compares a new build against it.  A
second table below gives the executed instruction bytes and the static
code size against rv32imac.

Columns: baseline and current executed instructions and their ratio;
current non-dataflow instructions (everything but `echo`, `dup` and
`nop`) and their ratio to the baseline's; share of dataflow instructions;
mean distance (instructions) between a `jmp` and its trigger; mean `ld`
output reference (slot loads, built in front of their consumer, are
left out); mean and maximum number of operands in flight in the
current frame; instructions executed by the same window on rv32imac
(Spike, GCC -O2) and the ratios of the current total and non-dataflow
counts to it.

| benchmark | baseline | current | ratio | non-DF | ratio | DF | jmp dist | ld ref | in flight | rv32imac | /rv32 | non-DF/rv32 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 24,356,988 | 5,324,355 | 0.22 | 3,428,300 | 0.17 | 36% | 40 | 11.1 | 6.0 / 18 | 5,063,219 | 1.1 | 0.7 |
| crc32 | 24,912,394 | 8,016,581 | 0.32 | 6,272,369 | 0.29 | 22% | 14 | 3.0 | 2.2 / 4 | 4,005,915 | 2.0 | 1.6 |
| depthconv | 19,961,445 | 8,144,232 | 0.41 | 4,777,715 | 0.27 | 41% | 487 | 22.2 | 53.7 / 78 | 3,463,226 | 2.4 | 1.4 |
| edn | 36,452,182 | 9,842,737 | 0.27 | 6,156,418 | 0.20 | 37% | 57 | 13.3 | 14.2 / 99 | 3,259,019 | 3.0 | 1.9 |
| matmult-int | 22,864,569 | 3,935,333 | 0.17 | 2,160,901 | 0.11 | 45% | 378 | 16.2 | 75.6 / 115 | 3,237,508 | 1.2 | 0.7 |
| md5sum | 26,396,891 | 9,551,541 | 0.36 | 4,707,594 | 0.21 | 51% | 20 | 16.9 | 9.3 / 21 | 3,121,844 | 3.1 | 1.5 |
| nettle-aes | 31,484,596 | 8,349,155 | 0.27 | 4,120,582 | 0.16 | 51% | 126 | 23.1 | 19.7 / 31 | 4,382,897 | 1.9 | 0.9 |
| nettle-sha256 | 69,905,081 | 10,678,847 | 0.15 | 6,312,064 | 0.11 | 41% | 346 | 16.8 | 33.6 / 57 | 5,155,822 | 2.1 | 1.2 |
| nsichneu | 17,034,943 | 6,140,403 | 0.36 | 5,510,842 | 0.37 | 10% | 0 | 0.5 | 1.5 / 4 | 2,242,272 | 2.7 | 2.5 |
| picojpeg | 35,334,763 | 8,204,717 | 0.23 | 5,039,423 | 0.17 | 39% | 44 | 5.8 | 12.9 / 33 | 3,209,092 | 2.6 | 1.6 |
| qrduino | 35,476,913 | 7,658,528 | 0.22 | 4,425,624 | 0.15 | 42% | 6 | 3.9 | 5.1 / 66 | 2,816,424 | 2.7 | 1.6 |
| sglib-combined | 40,141,033 | 10,335,282 | 0.26 | 6,750,650 | 0.19 | 35% | 5 | 1.1 | 3.6 / 34 | 2,779,205 | 3.7 | 2.4 |
| slre | 22,370,915 | 9,110,952 | 0.41 | 6,503,607 | 0.33 | 29% | 2 | 1.5 | 3.0 / 9 | 2,718,500 | 3.4 | 2.4 |
| statemate | 25,527,859 | 9,853,681 | 0.39 | 7,962,232 | 0.36 | 19% | 26 | 3.8 | 9.0 / 33 | 3,290,069 | 3.0 | 2.4 |
| tarfind | 25,816,534 | 3,484,245 | 0.13 | 2,343,717 | 0.11 | 33% | 15 | 2.5 | 4.3 / 21 | 2,031,177 | 1.7 | 1.2 |
| ud | 64,761,665 | 7,022,696 | 0.11 | 4,027,457 | 0.07 | 43% | 17 | 6.5 | 6.1 / 18 | 2,607,928 | 2.7 | 1.5 |
| xgboost | 32,029,599 | 8,713,233 | 0.27 | 4,149,881 | 0.15 | 52% | 7 | 2.8 | 5.0 / 20 | 3,559,528 | 2.4 | 1.2 |
| **total** | 554,828,370 | 134,366,518 | 0.24 | 84,649,376 | 0.18 | 37% | | | | 56,943,645 | 2.36 | 1.49 |

Geometric means of the per-benchmark ratios: current/baseline 0.250
(non-dataflow 0.181); current/rv32imac 2.33
(non-dataflow 1.45).

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
| aha-mont64 | 10,648,710 | 16,645,756 | 0.64 | 36% | 3,116 | 1,798 | 1.73 |
| crc32 | 16,033,162 | 11,495,140 | 1.39 | 57% | 480 | 314 | 1.53 |
| depthconv | 16,288,464 | 11,909,014 | 1.37 | 28% | 2,158 | 482 | 4.48 |
| edn | 19,685,474 | 10,179,178 | 1.93 | 44% | 8,456 | 10,912 | 0.77 |
| matmult-int | 7,870,666 | 9,226,478 | 0.85 | 58% | 7,726 | 9,774 | 0.79 |
| md5sum | 19,103,082 | 9,174,638 | 2.08 | 53% | 5,314 | 10,294 | 0.52 |
| nettle-aes | 16,698,310 | 14,398,330 | 1.16 | 36% | 6,998 | 3,192 | 2.19 |
| nettle-sha256 | 21,357,694 | 17,813,228 | 1.20 | 27% | 12,158 | 15,234 | 0.80 |
| nsichneu | 12,280,806 | 8,074,614 | 1.52 | 20% | 21,450 | 16,144 | 1.33 |
| picojpeg | 16,409,434 | 10,154,170 | 1.62 | 42% | 55,448 | 22,296 | 2.49 |
| qrduino | 15,317,056 | 9,280,310 | 1.65 | 35% | 33,622 | 18,394 | 1.83 |
| sglib-combined | 20,670,564 | 8,079,658 | 2.56 | 55% | 12,286 | 12,096 | 1.02 |
| slre | 18,221,904 | 7,756,316 | 2.35 | 57% | 9,312 | 12,294 | 0.76 |
| statemate | 19,707,362 | 11,901,482 | 1.66 | 19% | 10,630 | 15,026 | 0.71 |
| tarfind | 6,968,490 | 6,607,914 | 1.05 | 37% | 2,918 | 9,828 | 0.30 |
| ud | 14,045,392 | 7,743,432 | 1.81 | 52% | 4,268 | 10,206 | 0.42 |
| xgboost | 17,426,466 | 12,098,218 | 1.44 | 30% | 1,036 | 562 | 1.84 |
| **total** | 268,733,036 | 182,537,876 | 1.47 | 40% | 197,376 | 168,846 | 1.17 |

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
estimated rather than built: with the image at 0x100000 a symbol
address costs three instructions, and `ld [gp, idx]` would need one or
two for the index plus a `dup` to keep the pointer in flight per use,
so it converts about one non-dataflow instruction per global access
into a dataflow one without reducing the count.

Round 3 was measured with execution profiles (`scryer-metrics
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
