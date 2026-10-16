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
output reference (slot loads, built in front of their consumer, are
left out); mean and maximum number of operands in flight in the
current frame; instructions executed by the same window on rv32imac
(Spike, GCC -O2) and the ratios of the current total and non-dataflow
counts to it.

| benchmark | baseline | current | ratio | non-DF | ratio | DF | jmp dist | ld ref | in flight | rv32imac | /rv32 | non-DF/rv32 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 24,356,988 | 6,610,084 | 0.27 | 3,845,548 | 0.19 | 42% | 17 | 11.1 | 4.7 / 18 | 5,063,219 | 1.3 | 0.8 |
| crc32 | 24,912,394 | 8,016,581 | 0.32 | 6,272,369 | 0.29 | 22% | 14 | 3.0 | 2.2 / 4 | 4,005,915 | 2.0 | 1.6 |
| depthconv | 19,961,445 | 8,268,796 | 0.41 | 4,649,873 | 0.27 | 44% | 131 | 21.5 | 19.8 / 26 | 3,463,226 | 2.4 | 1.3 |
| edn | 36,452,182 | 11,414,269 | 0.31 | 7,092,262 | 0.23 | 38% | 22 | 7.1 | 7.0 / 38 | 3,259,019 | 3.5 | 2.2 |
| matmult-int | 22,864,569 | 4,818,593 | 0.21 | 2,011,441 | 0.10 | 58% | 52 | 22.2 | 36.8 / 54 | 3,237,508 | 1.5 | 0.6 |
| md5sum | 26,396,891 | 10,267,179 | 0.39 | 5,083,530 | 0.22 | 50% | 15 | 16.5 | 8.7 / 15 | 3,121,844 | 3.3 | 1.6 |
| nettle-aes | 31,484,596 | 8,546,952 | 0.27 | 4,231,359 | 0.16 | 50% | 94 | 23.1 | 19.3 / 31 | 4,382,897 | 2.0 | 1.0 |
| nettle-sha256 | 69,905,081 | 10,872,175 | 0.16 | 6,396,926 | 0.11 | 41% | 166 | 16.8 | 33.0 / 57 | 5,155,822 | 2.1 | 1.2 |
| nsichneu | 17,034,943 | 6,458,259 | 0.38 | 5,828,698 | 0.39 | 10% | 0 | 0.5 | 1.5 / 4 | 2,242,272 | 2.9 | 2.6 |
| picojpeg | 35,334,763 | 8,775,857 | 0.25 | 5,386,678 | 0.18 | 39% | 29 | 5.9 | 9.1 / 27 | 3,209,092 | 2.7 | 1.7 |
| qrduino | 35,476,913 | 8,346,318 | 0.24 | 4,639,849 | 0.15 | 44% | 6 | 3.6 | 5.1 / 27 | 2,816,424 | 3.0 | 1.6 |
| sglib-combined | 40,141,033 | 10,441,056 | 0.26 | 6,817,395 | 0.19 | 35% | 5 | 1.1 | 3.6 / 8 | 2,779,205 | 3.8 | 2.5 |
| slre | 22,370,915 | 9,255,372 | 0.41 | 6,648,027 | 0.34 | 28% | 2 | 1.5 | 3.0 / 9 | 2,718,500 | 3.4 | 2.4 |
| statemate | 25,527,859 | 9,886,981 | 0.39 | 7,912,282 | 0.36 | 20% | 23 | 3.8 | 9.1 / 33 | 3,290,069 | 3.0 | 2.4 |
| tarfind | 25,816,534 | 4,765,759 | 0.18 | 2,910,989 | 0.13 | 39% | 12 | 2.5 | 3.9 / 8 | 2,031,177 | 2.3 | 1.4 |
| ud | 64,761,665 | 7,022,696 | 0.11 | 4,027,457 | 0.07 | 43% | 17 | 6.5 | 6.1 / 18 | 2,607,928 | 2.7 | 1.5 |
| xgboost | 32,029,599 | 8,896,515 | 0.28 | 4,333,163 | 0.16 | 51% | 7 | 2.8 | 5.0 / 20 | 3,559,528 | 2.5 | 1.2 |
| **total** | 554,828,370 | 142,663,442 | 0.26 | 88,087,846 | 0.18 | 38% | | | | 56,943,645 | 2.51 | 1.55 |

Geometric means of the per-benchmark ratios: current/baseline 0.269
(non-dataflow 0.189); current/rv32imac 2.51
(non-dataflow 1.52).

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
