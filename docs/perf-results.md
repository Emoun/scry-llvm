# Performance results

Executed-instruction metrics of the current backend on the 17 Embench-IoT
benchmarks that run on scryer, measured like the baseline
(`perf-baseline.md`): with `scryer-metrics` over the benchmark window
between `start_trigger` and `stop_trigger`, runtime library included.
The raw data is `perf-results.json`; `scripts/compare_metrics.py
docs/perf-results.json new.json` compares a new build against it.

Columns: baseline and current executed instructions and their ratio;
current non-dataflow instructions (everything but `echo`, `dup` and
`nop`) and their ratio to the baseline's; share of dataflow instructions;
mean distance (instructions) between a `jmp` and its trigger; mean `ld`
output reference; mean and maximum number of operands in flight in the
current frame; instructions executed by the same window on rv32imac
(Spike, GCC -O2) and the ratios of the current total and non-dataflow
counts to it.

| benchmark | baseline | current | ratio | non-DF | ratio | DF | jmp dist | ld ref | in flight | rv32imac | /rv32 | non-DF/rv32 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 24,356,988 | 6,821,545 | 0.28 | 4,028,212 | 0.20 | 41% | 17 | 0.1 | 4.8 / 18 | 5,063,219 | 1.3 | 0.8 |
| crc32 | 24,912,394 | 8,016,581 | 0.32 | 6,272,369 | 0.29 | 22% | 14 | 1.5 | 2.2 / 4 | 4,005,915 | 2.0 | 1.6 |
| depthconv | 19,961,445 | 9,265,308 | 0.46 | 4,649,873 | 0.27 | 50% | 149 | 4.2 | 19.4 / 26 | 3,463,226 | 2.7 | 1.3 |
| edn | 36,452,182 | 12,737,728 | 0.35 | 7,268,194 | 0.23 | 43% | 23 | 3.5 | 7.2 / 36 | 3,259,019 | 3.9 | 2.2 |
| matmult-int | 22,864,569 | 7,422,272 | 0.32 | 2,321,959 | 0.12 | 69% | 41 | 0.5 | 35.9 / 54 | 3,237,508 | 2.3 | 0.7 |
| md5sum | 26,396,891 | 11,395,387 | 0.43 | 5,544,874 | 0.24 | 51% | 13 | 1.9 | 8.2 / 15 | 3,121,844 | 3.7 | 1.8 |
| nettle-aes | 31,484,596 | 10,770,311 | 0.34 | 4,319,258 | 0.17 | 60% | 116 | 4.1 | 20.1 / 31 | 4,382,897 | 2.5 | 1.0 |
| nettle-sha256 | 69,905,081 | 18,050,457 | 0.26 | 6,631,727 | 0.11 | 63% | 164 | 2.5 | 35.1 / 57 | 5,155,822 | 3.5 | 1.3 |
| nsichneu | 17,034,943 | 6,458,259 | 0.38 | 5,828,698 | 0.39 | 10% | 0 | 0.2 | 1.5 / 4 | 2,242,272 | 2.9 | 2.6 |
| picojpeg | 35,334,763 | 8,892,487 | 0.25 | 5,330,628 | 0.18 | 40% | 29 | 2.4 | 8.8 / 27 | 3,209,092 | 2.8 | 1.7 |
| qrduino | 35,476,913 | 8,905,463 | 0.25 | 5,073,119 | 0.17 | 43% | 6 | 1.3 | 5.2 / 27 | 2,816,424 | 3.2 | 1.8 |
| sglib-combined | 40,141,033 | 9,841,875 | 0.25 | 6,257,274 | 0.18 | 36% | 5 | 0.8 | 3.5 / 8 | 2,779,205 | 3.5 | 2.3 |
| slre | 22,370,915 | 9,515,792 | 0.43 | 6,648,375 | 0.34 | 30% | 2 | 0.5 | 3.0 / 9 | 2,718,500 | 3.5 | 2.4 |
| statemate | 25,527,859 | 11,395,422 | 0.45 | 8,488,323 | 0.38 | 26% | 17 | 0.9 | 9.3 / 51 | 3,290,069 | 3.5 | 2.6 |
| tarfind | 25,816,534 | 7,137,105 | 0.28 | 3,575,183 | 0.16 | 50% | 6 | 0.8 | 3.7 / 8 | 2,031,177 | 3.5 | 1.8 |
| ud | 64,761,665 | 8,341,588 | 0.13 | 4,923,292 | 0.09 | 41% | 14 | 1.0 | 4.1 / 10 | 2,607,928 | 3.2 | 1.9 |
| xgboost | 32,029,599 | 9,571,559 | 0.30 | 4,802,255 | 0.18 | 50% | 7 | 0.7 | 5.0 / 20 | 3,559,528 | 2.7 | 1.3 |
| **total** | 554,828,370 | 164,539,139 | 0.30 | 91,963,613 | 0.19 | 44% | | | | 56,943,645 | 2.89 | 1.61 |

Geometric means of the per-benchmark ratios: current/baseline 0.309
(non-dataflow 0.199); current/rv32imac 2.89
(non-dataflow 1.60).

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
   change off.

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
