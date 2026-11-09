# ISA and compiler metrics

How the code the backend generates uses the Scry ISA, measured on the 19
Embench-IoT benchmarks (window between `start_trigger` and `stop_trigger`,
runtime library included; scryer 01216f0, llvm-project 22.1.8, `-O2`),
next to the same programs compiled for rv32imac with GCC `-O2` and run
under Spike.  `perf-results.md` gives the instruction counts against the
baseline; this document looks at code size, the instruction mix, how long
operands and control flow stay in flight, memory traffic and operands in
flight.

The dynamic figures come from the simulator's event stream
(`scripts/scry_metrics.py`), the static ones from decoding the linked
image (`scripts/scry_static.py`, which also weights every instruction by
the execution profile and whose execution-weighted opcode counts equal the
dynamic counts on all 19 benchmarks), the rv32imac ones from the Spike
instruction log (`scripts/rv32_metrics.py`).  The raw data is
`perf-results.json` and `perf-rv32.json`; the tables are printed by

```sh
python3 scripts/run_embench.py --metrics --profile --json docs/perf-results.json
python3 scripts/rv32_metrics.py --spike <spike> --json docs/perf-rv32.json <rv32 images>
python3 scripts/metrics_report.py docs/perf-results.json --rv32 docs/perf-rv32.json
```

Every total row pools the counts and histograms of all benchmarks, so its
means and medians are over all events, not averages of the rows.

Instruction classes: **control** `jmp`, `call`, `ret`; **dataflow**
`echo`, `dup`, `nop` (the instructions that only move or pad, the
definition used throughout these documents); **alu** `add sub mul div sh
and or xor eq lt gt isnar narto`; **cast/pick**; **const/grow**
(immediates and their extension); **memory** `ld =>N` and `st` to an
address; **frame** `ld [i]`, `st [i]` and `saddr`, the private stack-frame
slots; **stack** `rsrv` and `free`.  For rv32imac, **control** is every
branch, jump, call and return and **ld/st** every load and store.

## Code size

Scry instructions are two bytes, rv32imac mixes two-byte compressed and
four-byte instructions (41% of them compressed over the window).  The linked
images are within 8% of each other in bytes (geometric mean of the
per-benchmark ratios 1.05), so Scry needs 1.6 instructions for every
rv32imac instruction in the code: 111,262 against 68,556 static
instructions.  Where Scry is much larger (depthconv 4.5x, picojpeg 2.5x,
nettle-aes 2.2x) the hot functions are: depthconv's convolution kernel
is 1,704 bytes against 278 on rv32imac, unrolled loop bodies with their
deliveries.  Where it is much smaller (tarfind 0.30, ud 0.42, md5sum
0.52) the rv32 image links libgcc's soft-float and 64-bit division
routines (`__adddf3`, `__divdf3`, `__muldf3`, `__divdi3`, `__moddi3`:
over 5 KB in tarfind) that the Scry link does not pull in.  Executed
bytes are 1.53x rv32imac because 2.43x the instructions are executed.

| benchmark | Scry text bytes | Scry instrs | rv32imac text bytes | ratio | Scry executed bytes | rv32imac executed bytes | ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 3,116 | 1,558 | 1,798 | 1.73 | 10,648,710 | 16,645,756 | 0.64 |
| crc32 | 480 | 240 | 314 | 1.53 | 16,033,162 | 11,495,140 | 1.39 |
| depthconv | 2,158 | 1,079 | 482 | 4.48 | 16,288,464 | 11,909,014 | 1.37 |
| edn | 8,456 | 4,228 | 10,912 | 0.77 | 19,685,474 | 10,179,178 | 1.93 |
| huffbench | 8,814 | 4,407 | 12,096 | 0.73 | 21,510,308 | 7,197,748 | 2.99 |
| matmult-int | 7,726 | 3,863 | 9,774 | 0.79 | 7,870,666 | 9,226,478 | 0.85 |
| md5sum | 5,314 | 2,657 | 10,294 | 0.52 | 19,103,082 | 9,174,638 | 2.08 |
| nettle-aes | 6,998 | 3,499 | 3,192 | 2.19 | 16,698,310 | 14,398,330 | 1.16 |
| nettle-sha256 | 12,158 | 6,079 | 15,234 | 0.80 | 21,672,414 | 17,813,228 | 1.22 |
| nsichneu | 21,450 | 10,725 | 16,144 | 1.33 | 12,280,806 | 8,074,614 | 1.52 |
| picojpeg | 55,448 | 27,724 | 22,296 | 2.49 | 16,409,434 | 10,154,170 | 1.62 |
| qrduino | 33,622 | 16,811 | 18,394 | 1.83 | 15,317,056 | 9,280,310 | 1.65 |
| sglib-combined | 12,286 | 6,143 | 12,096 | 1.02 | 20,670,564 | 8,079,658 | 2.56 |
| slre | 9,312 | 4,656 | 12,294 | 0.76 | 18,221,904 | 7,756,316 | 2.35 |
| statemate | 10,630 | 5,315 | 15,026 | 0.71 | 19,707,362 | 11,901,482 | 1.66 |
| tarfind | 2,918 | 1,459 | 9,828 | 0.30 | 6,968,490 | 6,607,914 | 1.05 |
| ud | 4,268 | 2,134 | 10,206 | 0.42 | 14,045,392 | 7,743,432 | 1.81 |
| wikisort | 16,334 | 8,167 | 25,102 | 0.65 | 10,285,384 | 6,995,868 | 1.47 |
| xgboost | 1,036 | 518 | 562 | 1.84 | 17,426,466 | 12,098,218 | 1.44 |
| **total** | 222,524 | 111,262 | 206,044 | 1.08 | 300,843,448 | 196,731,492 | 1.53 |

Geometric means of the per-benchmark ratios: static bytes 1.05, executed bytes 1.52.

## Dynamic instruction mix

Over the whole window 37% of the executed instructions are dataflow
instructions (`echo` 19%, `dup` 15%, `nop` 3%), 21% arithmetic and
logic, 17% constants (`const` and the `grow` chains that build
addresses and wide immediates), 16% memory traffic (half of it frame
slots), 5% casts and picks and 4% control flow.  The ALU, cast and
pick, memory, frame and control instructions together, 46% of the count,
are the work a register machine would also have to do; the dataflow and
constant instructions, 54%, are the cost of the temporal encoding
(moving values to where and when they are consumed) and of the two-byte
format (an address or a 32-bit immediate takes `const` plus up to three
`grow`s).

Control flow is a smaller share than on rv32imac in absolute terms as
well: 4.9 million `jmp` and 0.35 million calls against 9.1 million
rv32imac branches, jumps, calls and returns on the same programs.  The
backend's jumps are issued once per block and its selective unrolling
removes loop back-edges, and the rv32 count includes the compare-and-
branch chains of the 64-bit helper routines (aha-mont64: 10% control on
rv32, 2% on Scry, which has 64-bit operands).  The benchmark with the
most control flow, nsichneu, is a chain of if-statements on globals: one
instruction in eight is a `jmp` on Scry, nearly one in two on rv32imac.

| benchmark | executed | control | dataflow | alu | cast/pick | const/grow | memory | frame | stack | rv32 control | rv32 ld/st |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 5,324,355 | 2.0% | 35.6% | 40.0% | 7.1% | 13.2% | 0.1% | 2.0% | 0.0% | 10.3% | 0.4% |
| crc32 | 8,016,581 | 6.5% | 21.8% | 19.6% | 0.0% | 41.3% | 6.5% | 4.4% | 0.0% | 13.1% | 13.0% |
| depthconv | 8,144,232 | 0.2% | 41.3% | 20.5% | 17.4% | 10.7% | 7.1% | 2.7% | 0.0% | 13.8% | 18.6% |
| edn | 9,842,737 | 1.4% | 37.5% | 23.5% | 16.7% | 7.2% | 8.8% | 4.8% | 0.0% | 10.2% | 28.7% |
| huffbench | 10,755,154 | 4.1% | 40.5% | 20.9% | 0.7% | 19.8% | 8.6% | 5.4% | 0.0% | 22.3% | 26.3% |
| matmult-int | 3,935,333 | 0.2% | 45.1% | 28.1% | 0.0% | 5.4% | 13.7% | 7.4% | 0.0% | 14.0% | 37.1% |
| md5sum | 9,551,541 | 3.1% | 50.7% | 19.2% | 1.2% | 12.6% | 3.1% | 10.1% | 0.0% | 15.3% | 15.8% |
| nettle-aes | 8,349,155 | 0.7% | 50.6% | 27.1% | 0.4% | 10.9% | 9.3% | 1.0% | 0.0% | 1.7% | 19.3% |
| nettle-sha256 | 10,836,207 | 0.3% | 40.7% | 38.8% | 0.0% | 16.5% | 2.5% | 1.1% | 0.1% | 3.3% | 15.6% |
| nsichneu | 6,140,403 | 12.6% | 10.3% | 17.7% | 10.1% | 9.2% | 20.0% | 20.1% | 0.0% | 44.9% | 54.9% |
| picojpeg | 8,204,717 | 2.3% | 38.6% | 19.5% | 7.7% | 18.7% | 8.5% | 4.5% | 0.2% | 10.7% | 27.4% |
| qrduino | 7,658,528 | 4.8% | 42.2% | 20.3% | 4.2% | 10.8% | 7.4% | 10.3% | 0.0% | 15.0% | 20.6% |
| sglib-combined | 10,335,282 | 6.6% | 34.7% | 14.8% | 5.0% | 20.8% | 11.2% | 6.5% | 0.4% | 25.8% | 36.6% |
| slre | 9,110,952 | 7.2% | 28.6% | 14.7% | 4.6% | 22.6% | 6.1% | 15.1% | 1.0% | 25.9% | 31.7% |
| statemate | 9,853,681 | 2.8% | 19.2% | 10.0% | 0.1% | 28.8% | 22.1% | 16.4% | 0.5% | 13.2% | 49.3% |
| tarfind | 3,484,245 | 5.2% | 32.7% | 20.1% | 5.2% | 25.6% | 5.5% | 5.5% | 0.1% | 27.7% | 26.8% |
| ud | 7,022,696 | 4.5% | 42.7% | 23.1% | 1.1% | 11.7% | 7.8% | 9.3% | 0.0% | 17.0% | 23.0% |
| wikisort | 5,142,692 | 4.9% | 34.8% | 18.5% | 5.2% | 26.6% | 4.8% | 4.5% | 0.6% | 16.1% | 25.8% |
| xgboost | 8,713,233 | 2.7% | 52.4% | 12.3% | 4.8% | 4.0% | 9.6% | 14.1% | 0.0% | 14.7% | 25.0% |
| **total** | 150,421,724 | 3.7% | 37.2% | 21.1% | 4.7% | 16.8% | 8.6% | 7.7% | 0.2% | 14.8% | 24.2% |

## Static instruction mix

The static mix of the linked images is close to the dynamic one, with
more constants (23% against 17%: address formation in cold code and in
the runtime) and fewer ALU instructions.  Only 55% of the static
instructions run at all in the benchmark window: the rest is code that is
referenced but not executed there, Embench's set-up and verification,
the runtime routines they pull in, and cold paths of the benchmark
itself.  Execution is concentrated: 20% of the static instructions account
for 90% of the executed ones, 7% in md5sum and 9% in picojpeg.

| benchmark | static instrs | control | dataflow | alu | cast/pick | const/grow | memory | frame | stack | executed once | for 90% of execution |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 1,558 | 2.8% | 34.1% | 32.1% | 4.4% | 20.9% | 1.3% | 4.0% | 0.4% | 94.0% | 635 (40.8%) |
| crc32 | 240 | 13.3% | 24.6% | 7.5% | 0.8% | 41.2% | 3.8% | 7.1% | 1.7% | 66.2% | 42 (17.5%) |
| depthconv | 1,079 | 3.1% | 33.7% | 13.3% | 10.1% | 29.5% | 5.3% | 4.4% | 0.6% | 83.5% | 560 (51.9%) |
| edn | 4,228 | 2.4% | 44.3% | 16.5% | 9.7% | 15.1% | 7.5% | 4.3% | 0.2% | 76.3% | 609 (14.4%) |
| huffbench | 4,407 | 4.9% | 39.8% | 15.7% | 0.9% | 22.9% | 8.2% | 7.1% | 0.5% | 75.5% | 478 (10.8%) |
| matmult-int | 3,863 | 2.4% | 40.2% | 16.3% | 0.3% | 24.6% | 9.0% | 6.8% | 0.3% | 60.5% | 909 (23.5%) |
| md5sum | 2,657 | 5.0% | 47.3% | 13.3% | 0.4% | 20.1% | 6.7% | 6.8% | 0.4% | 50.0% | 188 (7.1%) |
| nettle-aes | 3,499 | 2.7% | 39.6% | 19.5% | 2.1% | 21.5% | 7.9% | 6.1% | 0.6% | 74.7% | 960 (27.4%) |
| nettle-sha256 | 6,079 | 1.8% | 42.2% | 28.2% | 0.2% | 18.2% | 5.2% | 3.9% | 0.3% | 73.0% | 3,000 (49.4%) |
| nsichneu | 10,725 | 6.2% | 18.9% | 18.8% | 4.7% | 11.9% | 22.6% | 16.7% | 0.1% | 47.5% | 4,486 (41.8%) |
| picojpeg | 27,724 | 3.2% | 34.5% | 16.9% | 8.6% | 23.8% | 8.2% | 4.8% | 0.1% | 32.4% | 2,548 (9.2%) |
| qrduino | 16,811 | 3.6% | 41.4% | 17.2% | 3.7% | 20.0% | 8.2% | 5.8% | 0.1% | 68.0% | 1,687 (10.0%) |
| sglib-combined | 6,143 | 5.6% | 35.2% | 14.1% | 1.7% | 25.5% | 10.3% | 7.1% | 0.4% | 69.5% | 1,525 (24.8%) |
| slre | 4,656 | 6.9% | 25.1% | 13.8% | 3.4% | 27.8% | 8.3% | 12.1% | 2.6% | 52.5% | 840 (18.0%) |
| statemate | 5,315 | 5.9% | 13.6% | 8.3% | 0.3% | 46.3% | 15.2% | 9.6% | 0.8% | 43.0% | 1,778 (33.5%) |
| tarfind | 1,459 | 7.7% | 41.2% | 12.5% | 0.8% | 20.8% | 5.0% | 11.7% | 0.4% | 88.1% | 541 (37.1%) |
| ud | 2,134 | 4.0% | 30.4% | 13.8% | 0.3% | 30.1% | 10.2% | 10.4% | 0.7% | 59.8% | 378 (17.7%) |
| wikisort | 8,167 | 5.1% | 38.5% | 16.7% | 1.3% | 28.1% | 4.3% | 5.0% | 1.1% | 41.6% | 1,169 (14.3%) |
| xgboost | 518 | 6.4% | 35.5% | 8.3% | 4.2% | 24.9% | 6.6% | 12.5% | 1.5% | 84.9% | 71 (13.7%) |
| **total** | 111,262 | 4.2% | 34.6% | 16.9% | 4.2% | 23.1% | 9.4% | 7.2% | 0.4% | 54.6% | 22,404 (20.1%) |

## Operand lifetime

The lifetime of an operand is the number of instructions executed between
the one that produces it and the one that consumes it (0: consumed by the
next instruction), from the simulator's produce and consume events; every
operand is counted once, at its final consumer, so an `echo` chain counts
as one lifetime from the original producer.  The **ref** columns are the
output references as written in the code (the `=>N` field, 0 when the
output goes to the next instruction), weighted by execution.  Operands
produced per instruction are 0.70; **discarded** operands were produced
and dropped unconsumed (a value delivered on a path that did not need
it, or the unused half of a two-output instruction); **forwards** are the
operands a `const` or `grow` passes on untouched per instruction.

Half of all operands are consumed by the very next instruction and the
median lifetime is 1, but the mean is 20 and 10.5% of the operands live
32 or more instructions, beyond the 5-bit output reference, which is why
the backend needs reach hops (`echo` with a 10-bit reference) and why
loop-carried values in long bodies (matmult-int, depthconv, nettle-sha256,
nettle-aes) dominate the dataflow share.  In the code itself only 3.6% of
the written references exceed 31, but those sit in the hot loops.  The
discard rate of 4.2% is the cost of delivering values speculatively to
both successors of a block.

| benchmark | operands | per instr | mean | median | p90 | at 0 | ≥ 32 | ref mean | ref at 0 | discarded | forwards per instr |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 4,395,884 | 0.83 | 6.4 | 1 | 18 | 28% | 2.4% | 4.0 | 50% | 5.6% | 0.41 |
| crc32 | 6,271,333 | 0.78 | 3.3 | 0 | 5 | 60% | 5.7% | 1.3 | 67% | 2.8% | 0.20 |
| depthconv | 5,810,277 | 0.71 | 75.1 | 1 | 326 | 42% | 24.4% | 46.4 | 35% | 2.7% | 0.45 |
| edn | 7,592,735 | 0.77 | 17.3 | 1 | 45 | 44% | 17.2% | 11.5 | 41% | 2.2% | 0.36 |
| huffbench | 7,123,140 | 0.66 | 9.8 | 1 | 22 | 48% | 6.0% | 4.1 | 50% | 6.2% | 0.48 |
| matmult-int | 2,868,914 | 0.73 | 103.0 | 2 | 380 | 44% | 26.7% | 63.7 | 35% | 1.0% | 0.48 |
| md5sum | 5,203,418 | 0.54 | 14.5 | 2 | 58 | 34% | 17.8% | 8.0 | 32% | 5.6% | 0.50 |
| nettle-aes | 5,785,322 | 0.69 | 29.2 | 4 | 105 | 29% | 24.4% | 15.7 | 28% | 2.3% | 0.49 |
| nettle-sha256 | 8,931,499 | 0.82 | 46.3 | 3 | 71 | 38% | 13.4% | 26.2 | 34% | 0.9% | 0.40 |
| nsichneu | 5,046,349 | 0.82 | 0.5 | 0 | 2 | 72% | 0.0% | 0.7 | 72% | 6.2% | 0.05 |
| picojpeg | 5,528,228 | 0.67 | 18.3 | 1 | 49 | 41% | 12.7% | 11.2 | 44% | 4.1% | 0.40 |
| qrduino | 4,891,899 | 0.64 | 6.3 | 1 | 17 | 46% | 6.0% | 3.9 | 47% | 8.2% | 0.43 |
| sglib-combined | 6,937,492 | 0.67 | 4.6 | 0 | 12 | 52% | 2.9% | 2.6 | 57% | 4.9% | 0.42 |
| slre | 5,903,983 | 0.65 | 8.7 | 0 | 7 | 59% | 2.2% | 2.3 | 62% | 4.2% | 0.29 |
| statemate | 7,099,715 | 0.72 | 12.3 | 0 | 11 | 63% | 8.0% | 8.9 | 74% | 1.6% | 0.39 |
| tarfind | 2,467,469 | 0.71 | 7.5 | 0 | 14 | 56% | 7.7% | 3.3 | 56% | 2.4% | 0.30 |
| ud | 4,557,521 | 0.65 | 8.7 | 1 | 22 | 42% | 5.7% | 4.9 | 41% | 5.7% | 0.40 |
| wikisort | 3,556,124 | 0.69 | 10.7 | 0 | 24 | 54% | 6.0% | 3.8 | 53% | 2.9% | 0.37 |
| xgboost | 5,267,106 | 0.60 | 9.0 | 2 | 31 | 18% | 9.7% | 3.5 | 38% | 11.9% | 0.52 |
| **total** | 105,238,408 | 0.70 | 20.1 | 1 | 33 | 46% | 10.5% | 11.8 | 47% | 4.2% | 0.39 |

## Control-flow lifetime

A `jmp` is issued where its condition is known and triggers at a later
instruction (its trigger field, 6 bits in the near form, the target and
trigger fields combined in the far form).  A jump whose condition does not
hold issues nothing (**not taken**); when both successors of a block need
a jump the backend issues the unconditional one first and a conditional
one with the same trigger point, which **overrides** it when taken, so
the overridden jumps are issued and never trigger.  **Taken** is the
share of executed `jmp`s that triggered.  The trigger field is the
distance written in the code; **issue to trigger** counts executed
instructions, so a call executed between issue and trigger adds the
callee.  Calls and returns have their own trigger field; the backend
issues them with distance 0 throughout.

Half of the executed jumps are taken and 2.4% are overridden defaults.
The mean distance between a jump and its trigger is 17 instructions as
written and 54 as executed (the long loop bodies of depthconv,
matmult-int and nettle-sha256 keep their exit jumps pending for hundreds
of instructions), the median 5.  27% of the taken jumps trigger at the
next instruction, so a quarter of the control flow gets no lookahead at
all: nsichneu (every jump, the condition is the loaded flag itself),
statemate (71%) and slre (40%).  18% of the jumps trigger 32 or more
instructions ahead; only 3.7% of the executed jumps carry a distance over
63, the limit of the near form.  The call and return trigger distance is
unused: every call and return triggers immediately, which is the one
lifetime the backend does not yet exploit.

| benchmark | jmp per 1000 | taken | overridden | trigger field mean | issue→trigger mean | median | at 0 | ≥ 32 | calls per 1000 | call/ret trigger mean | max depth |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 19.9 | 70.1% | 0.0% | 39.8 | 52.9 | 90 | 0% | 57% | 0.0 | 0.00 | 3 |
| crc32 | 21.8 | 99.6% | 0.0% | 13.9 | 40.0 | 40 | 0% | 100% | 21.7 | 0.00 | 4 |
| depthconv | 1.8 | 88.9% | 0.0% | 486.5 | 1098.8 | 547 | 0% | 100% | 0.2 | 0.00 | 4 |
| edn | 14.4 | 93.0% | 0.1% | 56.8 | 55.5 | 30 | 1% | 20% | 0.0 | 0.00 | 4 |
| huffbench | 40.6 | 54.1% | 0.3% | 7.5 | 54.0 | 4 | 16% | 1% | 0.0 | 0.00 | 5 |
| matmult-int | 2.4 | 87.4% | 0.4% | 378.3 | 350.1 | 45 | 4% | 86% | 0.0 | 0.00 | 4 |
| md5sum | 30.8 | 47.8% | 0.0% | 19.7 | 11.7 | 9 | 37% | 15% | 0.0 | 0.00 | 5 |
| nettle-aes | 7.0 | 69.6% | 0.0% | 126.1 | 354.5 | 197 | 10% | 80% | 0.0 | 0.00 | 4 |
| nettle-sha256 | 2.9 | 67.9% | 1.8% | 327.4 | 805.3 | 34 | 18% | 68% | 0.2 | 0.00 | 4 |
| nsichneu | 126.0 | 20.4% | 0.2% | 0.0 | 0.0 | 0 | 99% | 0% | 0.0 | 0.00 | 3 |
| picojpeg | 22.0 | 62.8% | 1.2% | 44.5 | 44.2 | 8 | 17% | 26% | 0.7 | 0.00 | 7 |
| qrduino | 47.6 | 44.3% | 2.1% | 5.8 | 54.2 | 2 | 23% | 3% | 0.0 | 0.00 | 6 |
| sglib-combined | 60.5 | 42.4% | 6.8% | 5.3 | 6.1 | 2 | 36% | 2% | 2.8 | 0.00 | 12 |
| slre | 69.3 | 46.1% | 4.8% | 2.3 | 34.0 | 1 | 40% | 1% | 1.6 | 0.00 | 8 |
| statemate | 24.0 | 59.2% | 1.4% | 25.7 | 90.6 | 0 | 71% | 14% | 2.0 | 0.00 | 5 |
| tarfind | 31.1 | 58.5% | 6.2% | 14.9 | 31.7 | 53 | 18% | 54% | 10.7 | 0.00 | 4 |
| ud | 34.1 | 52.2% | 8.2% | 16.8 | 21.3 | 11 | 0% | 9% | 5.3 | 0.00 | 4 |
| wikisort | 40.1 | 48.9% | 0.1% | 6.5 | 52.6 | 5 | 37% | 9% | 4.5 | 0.00 | 6 |
| xgboost | 27.1 | 77.7% | 0.1% | 6.5 | 7.8 | 3 | 0% | 0% | 0.0 | 0.00 | 4 |
| **total** | 32.4 | 50.1% | 2.4% | 16.8 | 53.6 | 5 | 27% | 18% | 2.3 | 0.00 | 12 |

## Memory traffic

Loads and stores per 1,000 executed instructions, the bytes each one
moves, the share that goes to the private frame slots rather than to an
address, and the distance from a memory load to the first use of its
value (frame-slot loads are built in front of their consumer by design and
are left out).  The rv32imac columns count its loads and stores the same
way; its stack accesses are ordinary loads and stores and cannot be told
apart.

In absolute terms Scry executes 16.7 million loads against 10.3 million
on rv32imac and 4.9 against 4.7 million stores.  41% of the Scry loads
and 37% of the stores are frame slots (6.8 and 1.8 million): without
registers, a value that is live across a block boundary or a call and
cannot stay in flight goes through the frame, and 64-bit values go as a
whole (aha-mont64: 96% of its loads are frame slots of 8 bytes).  The
addressed loads alone, 9.9 million, are fewer than the rv32imac total,
which also contains its stack traffic.  Loads move 3.4 bytes and stores
3.3 bytes on average; the runtime's eight-byte copy loops lift wikisort
and aha-mont64 above 6.  The load-to-use distance has a mean of 48 and a
median of 4: the scheduler issues loads early in their block, and the
long distances are the loop bodies of matmult-int, depthconv and
nettle-sha256 (median 87 to 185 instructions).

| benchmark | loads per 1000 | bytes per load | from frame slots | stores per 1000 | bytes per store | to frame slots | load→use mean | median | rv32 loads per 1000 | rv32 stores per 1000 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 17.0 | 7.9 | 96.4% | 3.3 | 7.8 | 94.6% | 93.1 | 55 | 2.5 | 1.2 |
| crc32 | 87.0 | 4.0 | 50.1% | 21.8 | 4.0 | 0.2% | 3.0 | 2 | 86.9 | 43.5 |
| depthconv | 88.5 | 2.3 | 27.3% | 9.5 | 2.0 | 31.9% | 208.5 | 181 | 168.5 | 17.0 |
| edn | 128.5 | 2.8 | 36.7% | 7.8 | 3.2 | 11.8% | 34.3 | 14 | 256.0 | 31.3 |
| huffbench | 56.6 | 3.3 | 0.3% | 30.1 | 3.4 | 0.2% | 6.8 | 2 | 188.8 | 74.3 |
| matmult-int | 161.0 | 4.1 | 19.8% | 8.2 | 5.9 | 0.8% | 260.2 | 185 | 231.3 | 139.7 |
| md5sum | 79.9 | 4.0 | 72.2% | 52.1 | 3.7 | 83.0% | 25.8 | 28 | 89.4 | 68.6 |
| nettle-aes | 97.6 | 3.8 | 8.2% | 4.9 | 4.0 | 33.3% | 90.0 | 92 | 179.6 | 13.5 |
| nettle-sha256 | 21.1 | 3.7 | 16.0% | 10.2 | 3.3 | 28.1% | 340.6 | 87 | 102.9 | 52.7 |
| nsichneu | 400.9 | 4.0 | 50.2% | 1.0 | 4.0 | 40.1% | 1.3 | 0 | 547.2 | 1.7 |
| picojpeg | 70.4 | 2.1 | 36.7% | 55.1 | 2.0 | 26.3% | 23.2 | 9 | 144.1 | 130.0 |
| qrduino | 139.3 | 2.9 | 54.6% | 29.8 | 2.7 | 64.7% | 5.6 | 4 | 180.2 | 25.4 |
| sglib-combined | 89.8 | 3.7 | 6.7% | 33.8 | 3.7 | 16.3% | 2.5 | 1 | 245.2 | 121.2 |
| slre | 105.4 | 2.9 | 61.3% | 64.2 | 3.7 | 68.1% | 6.6 | 1 | 200.3 | 116.4 |
| statemate | 168.3 | 3.1 | 31.3% | 123.4 | 2.3 | 14.5% | 26.7 | 1 | 172.1 | 320.8 |
| tarfind | 62.9 | 4.4 | 74.4% | 47.4 | 4.3 | 17.6% | 2.7 | 4 | 27.7 | 240.3 |
| ud | 126.8 | 4.1 | 51.3% | 29.7 | 4.6 | 47.0% | 12.7 | 5 | 165.6 | 64.3 |
| wikisort | 46.7 | 6.3 | 39.0% | 29.6 | 6.8 | 34.8% | 4.5 | 1 | 164.7 | 93.3 |
| xgboost | 212.9 | 2.6 | 54.8% | 24.4 | 3.5 | 99.2% | 3.5 | 4 | 235.7 | 14.8 |
| **total** | 110.9 | 3.4 | 40.8% | 32.7 | 3.3 | 36.6% | 47.7 | 4 | 166.7 | 75.3 |

## Operands in flight

Operands produced and not yet consumed after each executed instruction,
in the current frame and in all frames (callers' values waiting for the
return included).  The mean is 11.9 per frame; the peaks are the unrolled
loop bodies (matmult-int 113, edn 97, depthconv 78), the floor the
branchy state machines (nsichneu 0.2, slre 0.4).

| benchmark | frame mean | frame max | all mean | all max |
|---|---:|---:|---:|---:|
| aha-mont64 | 4.0 | 16 | 6.0 | 18 |
| crc32 | 1.3 | 3 | 3.3 | 5 |
| depthconv | 53.7 | 78 | 55.7 | 80 |
| edn | 12.1 | 97 | 14.2 | 100 |
| huffbench | 3.4 | 31 | 8.4 | 37 |
| matmult-int | 73.6 | 113 | 76.2 | 115 |
| md5sum | 7.2 | 18 | 9.4 | 26 |
| nettle-aes | 15.8 | 27 | 21.8 | 34 |
| nettle-sha256 | 30.3 | 54 | 42.1 | 66 |
| nsichneu | 0.2 | 3 | 1.5 | 4 |
| picojpeg | 12.7 | 33 | 13.1 | 33 |
| qrduino | 4.9 | 66 | 6.4 | 67 |
| sglib-combined | 1.8 | 31 | 4.1 | 35 |
| slre | 0.4 | 7 | 9.3 | 18 |
| statemate | 8.6 | 33 | 10.4 | 34 |
| tarfind | 2.6 | 18 | 6.2 | 24 |
| ud | 4.2 | 16 | 6.6 | 18 |
| wikisort | 3.0 | 11 | 8.4 | 21 |
| xgboost | 4.0 | 19 | 6.0 | 21 |
| **total** | 11.9 | 113 | 15.5 | 115 |

## Reading the figures

For the ISA: a two-byte instruction gives images the size of rv32imac's
with 1.6 instructions per rv32imac instruction, and executes 2.4 times as
many; the difference is the dataflow and constant instructions, 54% of
the dynamic count.  Half of the operand lifetimes fit the next
instruction and 90% fit the 5-bit reference; the remaining tenth, and
the 32-bit constants, are where the encoding costs most.  Control flow is
issued on average 17 instructions ahead of its trigger, a quarter of it
with no lookahead.

For the compiler: the discard rate (4.2%), the forwards (0.39 per
instruction, the `grow` chains), the frame-slot traffic (41% of loads)
and the unused call and return trigger distance are the four figures the
backend could still move; the operand lifetimes beyond 31 and the
in-flight peaks are properties of the unrolled loops that trade
dataflow instructions for fewer jumps.
