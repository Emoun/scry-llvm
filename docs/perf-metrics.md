# ISA and compiler metrics

How the code the backend generates uses the Scry ISA, measured on the 19
Embench-IoT benchmarks (window between `start_trigger` and `stop_trigger`,
runtime library included; scryer 01216f0, llvm-project 22.1.8, `-O2`),
next to the same programs compiled for rv32imac by the same LLVM
(`scripts/build_rv32.py`: clang 22.1.8 with the RISC-V target, lld, this
repository's libc and compiler-rt builtins, `-O2`) and run under Spike.
`perf-results.md` gives the instruction counts against the
baseline; this document looks at code size, the instruction mix, what
the constants are for, how long operands and control flow stay in
flight, memory traffic and operands in flight.

The dynamic figures come from the simulator's event stream
(`scripts/scry_metrics.py`), the static ones from decoding the linked
image (`scripts/scry_static.py`, which also weights every instruction by
the execution profile and whose execution-weighted opcode counts equal the
dynamic counts on all 19 benchmarks), the rv32imac ones from the Spike
instruction log (`scripts/rv32_metrics.py`).  The raw data is
`perf-results.json` and `perf-rv32.json`; the tables are printed by

```sh
python3 scripts/run_embench.py --metrics --profile --json docs/perf-results.json
python3 scripts/build_rv32.py --spike build/spike/bin/spike --json docs/perf-rv32.json
python3 scripts/metrics_report.py docs/perf-results.json --rv32 docs/perf-rv32.json
python3 scripts/rv32_memory.py --spike build/spike/bin/spike --llvm-bin install/bin \
    --json docs/perf-rv32-memory.json build/rv32/*.elf
```

Every **pooled** row sums the counts and histograms of all benchmarks,
so its means, shares and quantiles are over all events, not averages of
the rows.  The code-size table, which compares the two ISAs, ends in a
**geometric mean** row instead: the mean over the benchmarks of each
column, so that its ratio cells are also the mean of the per-benchmark
ratios; the pooled sums follow that table in a sentence.

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
four-byte instructions (45% of them compressed over the window, 3.1 bytes
per executed instruction).  The linked images are 2.8 times the rv32imac
ones in bytes (geometric mean over the benchmarks; pooled 2.87), so Scry
needs 4.0 instructions for every rv32imac instruction in the code
(pooled 4.2: 111,262 against 26,375 static instructions).  The ratio is
smallest where
the code is straight-line or 64-bit (aha-mont64 1.3, nsichneu 1.9) and
largest in the loop kernels that the backend unrolls by four and whose
bodies carry their deliveries (matmult-int 6.0, edn 4.8, depthconv 4.3):
depthconv's convolution kernel is 1,704 bytes against 286 on rv32imac,
edn's `jpegdct` 3,348 against 516.  The runtime weighs in the same way:
the eight-byte `memcpy` is 2,332 bytes on Scry and 296 on rv32imac.
Executed bytes are 1.8x rv32imac because 2.8x the instructions are
executed (geometric means; pooled 1.75x and 2.71x).

| benchmark | Scry text bytes | Scry instrs | rv32imac text bytes | ratio | Scry executed bytes | rv32imac executed bytes | ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 3,116 | 1,558 | 2,364 | 1.32 | 10,648,710 | 20,001,766 | 0.53 |
| crc32 | 480 | 240 | 324 | 1.48 | 16,033,162 | 10,799,840 | 1.48 |
| depthconv | 2,158 | 1,079 | 506 | 4.26 | 16,288,464 | 9,270,234 | 1.76 |
| edn | 8,456 | 4,228 | 1,748 | 4.84 | 19,685,474 | 11,744,992 | 1.68 |
| huffbench | 8,814 | 4,407 | 2,300 | 3.83 | 21,510,308 | 7,062,252 | 3.05 |
| matmult-int | 7,726 | 3,863 | 1,288 | 6.00 | 7,870,666 | 3,949,258 | 1.99 |
| md5sum | 5,314 | 2,657 | 1,350 | 3.94 | 19,103,082 | 6,911,360 | 2.76 |
| nettle-aes | 6,998 | 3,499 | 2,972 | 2.35 | 16,698,310 | 11,813,894 | 1.41 |
| nettle-sha256 | 12,158 | 6,079 | 6,304 | 1.93 | 21,672,414 | 15,845,124 | 1.37 |
| nsichneu | 21,450 | 10,725 | 11,142 | 1.93 | 12,280,806 | 6,379,356 | 1.93 |
| picojpeg | 55,448 | 27,724 | 16,742 | 3.31 | 16,409,434 | 8,747,590 | 1.88 |
| qrduino | 33,622 | 16,811 | 10,216 | 3.29 | 15,317,056 | 9,208,914 | 1.66 |
| sglib-combined | 12,286 | 6,143 | 3,142 | 3.91 | 20,670,564 | 7,363,012 | 2.81 |
| slre | 9,312 | 4,656 | 3,266 | 2.85 | 18,221,904 | 8,363,678 | 2.18 |
| statemate | 10,630 | 5,315 | 3,724 | 2.85 | 19,707,362 | 6,873,206 | 2.87 |
| tarfind | 2,918 | 1,459 | 1,062 | 2.75 | 6,968,490 | 3,748,936 | 1.86 |
| ud | 4,268 | 2,134 | 1,304 | 3.27 | 14,045,392 | 7,811,304 | 1.80 |
| wikisort | 16,334 | 8,167 | 6,966 | 2.34 | 10,285,384 | 4,998,950 | 2.06 |
| xgboost | 1,036 | 518 | 752 | 1.38 | 17,426,466 | 10,920,064 | 1.60 |
| **geometric mean** | 6,897 | 3,449 | 2,460 | 2.80 | 15,118,850 | 8,307,525 | 1.82 |

Pooled over the benchmarks: 222,524 against 77,472 bytes of code (ratio 2.87), 111,262 against 26,375 static instructions (4.22), 300,843,448 against 171,813,730 executed bytes (1.75).

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
well: 4.9 million `jmp` and 0.35 million calls against 7.2 million
rv32imac branches, jumps, calls and returns on the same programs.  The
backend's jumps are issued once per block and its selective unrolling
removes loop back-edges, and the rv32 count includes the compare-and-
branch chains of the 64-bit helper routines (aha-mont64: 7.6% control on
rv32, 2% on Scry, which has 64-bit operands).  The benchmark with the
most control flow, nsichneu, is a chain of if-statements on globals: one
instruction in eight is a `jmp` on Scry, more than one in three on
rv32imac.

| benchmark | executed | control | dataflow | alu | cast/pick | const/grow | memory | frame | stack | rv32 control | rv32 ld/st |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 5,324,355 | 2.0% | 35.6% | 40.0% | 7.1% | 13.2% | 0.1% | 2.0% | 0.0% | 7.6% | 0.2% |
| crc32 | 8,016,581 | 6.5% | 21.8% | 19.6% | 0.0% | 41.3% | 6.5% | 4.4% | 0.0% | 13.7% | 13.6% |
| depthconv | 8,144,232 | 0.2% | 41.3% | 20.5% | 17.4% | 10.7% | 7.1% | 2.7% | 0.0% | 9.8% | 22.0% |
| edn | 9,842,737 | 1.4% | 37.5% | 23.5% | 16.7% | 7.2% | 8.8% | 4.8% | 0.0% | 9.3% | 25.5% |
| huffbench | 10,755,154 | 4.1% | 40.5% | 20.9% | 0.7% | 19.8% | 8.6% | 5.4% | 0.0% | 20.7% | 23.0% |
| matmult-int | 3,935,333 | 0.2% | 45.1% | 28.1% | 0.0% | 5.4% | 13.7% | 7.4% | 0.0% | 1.8% | 45.4% |
| md5sum | 9,551,541 | 3.1% | 50.7% | 19.2% | 1.2% | 12.6% | 3.1% | 10.1% | 0.0% | 13.4% | 12.6% |
| nettle-aes | 8,349,155 | 0.7% | 50.6% | 27.1% | 0.4% | 10.9% | 9.3% | 1.0% | 0.0% | 2.2% | 24.1% |
| nettle-sha256 | 10,836,207 | 0.3% | 40.7% | 38.8% | 0.0% | 16.5% | 2.5% | 1.1% | 0.1% | 1.4% | 20.6% |
| nsichneu | 6,140,403 | 12.6% | 10.3% | 17.7% | 10.1% | 9.2% | 20.0% | 20.1% | 0.0% | 38.5% | 61.3% |
| picojpeg | 8,204,717 | 2.3% | 38.6% | 19.5% | 7.7% | 18.7% | 8.5% | 4.5% | 0.2% | 11.7% | 28.6% |
| qrduino | 7,658,528 | 4.8% | 42.2% | 20.3% | 4.2% | 10.8% | 7.4% | 10.3% | 0.0% | 15.1% | 19.1% |
| sglib-combined | 10,335,282 | 6.6% | 34.7% | 14.8% | 5.0% | 20.8% | 11.2% | 6.5% | 0.4% | 25.6% | 35.2% |
| slre | 9,110,952 | 7.2% | 28.6% | 14.7% | 4.6% | 22.6% | 6.1% | 15.1% | 1.0% | 25.3% | 34.8% |
| statemate | 9,853,681 | 2.8% | 19.2% | 10.0% | 0.1% | 28.8% | 22.1% | 16.4% | 0.5% | 15.9% | 72.1% |
| tarfind | 3,484,245 | 5.2% | 32.7% | 20.1% | 5.2% | 25.6% | 5.5% | 5.5% | 0.1% | 14.3% | 20.1% |
| ud | 7,022,696 | 4.5% | 42.7% | 23.1% | 1.1% | 11.7% | 7.8% | 9.3% | 0.0% | 12.2% | 22.7% |
| wikisort | 5,142,692 | 4.9% | 34.8% | 18.5% | 5.2% | 26.6% | 4.8% | 4.5% | 0.6% | 15.3% | 23.1% |
| xgboost | 8,713,233 | 2.7% | 52.4% | 12.3% | 4.8% | 4.0% | 9.6% | 14.1% | 0.0% | 16.4% | 24.9% |
| **pooled** | 150,421,724 | 3.7% | 37.2% | 21.1% | 4.7% | 16.8% | 8.6% | 7.7% | 0.2% | 13.0% | 24.2% |

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
| **pooled** | 111,262 | 4.2% | 34.6% | 16.9% | 4.2% | 23.1% | 9.4% | 7.2% | 0.4% | 54.6% | 22,404 (20.1%) |

## Constant groups

A constant group is a `const` and the `grow`s that extend it: one
immediate, delivered to the next instruction (several constants for one
instruction are built back to back, each forwarded behind the next
group's value).  The 16.8% of const/grow instructions are 17.6 million
groups, 117 per 1,000 instructions, 1.43 instructions each: 75% are a
single `const` (a value or offset that fits a byte), 15% two
instructions, 6.5% the four of a 32-bit value or an absolute address.
Following each value through the `echo` and `dup` hops to the
instruction that consumes it, the groups are used for:

* **ALU, 76%**: the immediates of `add`, `sub`, `and`, the shifts and
  the compares, which rv32imac folds into `addi`, `andi`, `slli` or a
  compare-and-branch with zero.  On Scry every immediate is at least one
  instruction; in the kernels of edn, matmult-int, nettle-sha256 and ud
  more than 94% of the groups are ALU operands.
* **control, 12%**: the target of a far-form jump, built by a chain
  because the near form's field does not reach, the polarity constant
  of an unconditional far jump, and the callee address of a call.  Calls
  are few (0.35 million against 2.1 million control groups), so these
  are almost all jump operands: nsichneu's one large function sends 44%
  of its groups to jumps, slre 29%, sglib-combined 23%, md5sum 21%.
* **loads and stores, 8%**: the address of a global and the constants
  that are stored.  statemate assigns constants to its global state
  (31% of its groups, 433,000, go to loads and stores), sglib-combined
  18%, tarfind 16%, xgboost 15%.
* **to a frame slot, 1.7%**: a hoisted constant the slot allocator gave
  a slot, built once and stored (slre 8.6%, statemate 6.7%).
* **discarded, 2.2%**: the operand a `pick` did not select, in
  aha-mont64 the zero of a `c ? x : 0` in the 64-bit carry code (12% of
  its groups), in depthconv, qrduino and slre 6 to 8%.
* **cast/pick, 0.2%**: the selected operand of a `pick` or the input of a
  `cast`.

4.8% of the groups pass through an `echo` or `dup` before their consumer:
constants that cannot be glued to it, such as call arguments or a value
needed on the far side of a jump (aha-mont64 12.5%, depthconv 16%,
slre 15%, xgboost 15%).  The other 95% are consumed by the instruction
after the group, as the emitter intends.

In the code, const/grow is 23% of the instructions (16,500 groups in
25,700 instructions): 59% feed ALU instructions, 15% control flow (every
call site is a three-instruction address chain, and cold code has far
jumps), 11.5% loads and stores, 3.3% a frame slot and 10% an `echo` or
`dup`.  statemate's code is 46% constants, 53% of them stored to
globals; crc32's small image is 41% constants because its call-address
chains and table address dominate the little code there is.

| benchmark | groups | per 1000 instrs | instrs per group | alu | control | ld/st | to slot | cast/pick | discarded | via echo/dup |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 700,485 | 131.6 | 1.01 | 77.7% | 10.4% | 0.0% | 0.1% | 0.1% | 11.7% | 12.5% |
| crc32 | 1,393,503 | 173.8 | 2.37 | 75.0% | 0.0% | 25.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| depthconv | 653,969 | 80.3 | 1.34 | 86.0% | 2.0% | 0.0% | 3.8% | 0.3% | 8.0% | 16.0% |
| edn | 694,552 | 70.6 | 1.03 | 95.4% | 4.6% | 0.0% | 0.0% | 0.0% | 0.0% | 3.8% |
| huffbench | 1,336,209 | 124.2 | 1.60 | 80.2% | 17.1% | 0.6% | 0.0% | 0.0% | 2.0% | 3.8% |
| matmult-int | 171,176 | 43.5 | 1.25 | 96.6% | 3.2% | 0.1% | 0.1% | 0.0% | 0.0% | 0.6% |
| md5sum | 1,158,994 | 121.3 | 1.04 | 78.6% | 20.8% | 0.1% | 0.5% | 0.0% | 0.0% | 0.6% |
| nettle-aes | 895,242 | 107.2 | 1.02 | 91.2% | 6.3% | 2.5% | 0.1% | 0.0% | 0.0% | 0.1% |
| nettle-sha256 | 1,777,103 | 164.0 | 1.00 | 97.3% | 0.9% | 1.6% | 0.1% | 0.0% | 0.1% | 0.5% |
| nsichneu | 556,889 | 90.7 | 1.01 | 55.7% | 43.6% | 0.7% | 0.0% | 0.0% | 0.0% | 0.0% |
| picojpeg | 1,189,769 | 145.0 | 1.29 | 73.5% | 11.8% | 7.4% | 3.6% | 0.3% | 3.4% | 3.7% |
| qrduino | 704,606 | 92.0 | 1.18 | 71.1% | 16.0% | 4.1% | 1.4% | 0.0% | 7.4% | 7.9% |
| sglib-combined | 1,463,485 | 141.6 | 1.47 | 55.8% | 22.9% | 18.1% | 0.0% | 0.4% | 2.8% | 7.3% |
| slre | 1,339,235 | 147.0 | 1.54 | 50.8% | 29.1% | 3.8% | 8.6% | 2.1% | 5.6% | 15.4% |
| statemate | 1,401,974 | 142.3 | 2.03 | 58.2% | 3.8% | 30.9% | 6.7% | 0.0% | 0.5% | 1.2% |
| tarfind | 488,138 | 140.1 | 1.83 | 81.5% | 1.8% | 15.9% | 0.5% | 0.0% | 0.3% | 2.0% |
| ud | 735,526 | 104.7 | 1.12 | 94.7% | 2.2% | 1.9% | 1.2% | 0.0% | 0.0% | 5.1% |
| wikisort | 628,629 | 122.2 | 2.18 | 83.2% | 14.7% | 1.5% | 0.0% | 0.0% | 0.5% | 4.9% |
| xgboost | 346,497 | 39.8 | 1.01 | 69.0% | 15.1% | 15.2% | 0.4% | 0.0% | 0.3% | 15.3% |
| **pooled** | 17,635,981 | 117.2 | 1.43 | 75.8% | 12.0% | 8.1% | 1.7% | 0.2% | 2.2% | 4.8% |

#### Constant groups in the code

Groups in the linked image and their instructions, the share of the code
they are, and the consumer by the instruction after the group (so
`echo`/`dup` here is the capture of a constant that is forwarded on).

| benchmark | groups | instrs | of the code | alu | control | ld/st | to slot | echo/dup | other |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 218 | 325 | 20.9% | 52.8% | 12.4% | 6.9% | 1.4% | 9.2% | 17.4% |
| crc32 | 43 | 99 | 41.2% | 18.6% | 25.6% | 20.9% | 7.0% | 27.9% | 0.0% |
| depthconv | 127 | 318 | 29.5% | 32.3% | 9.4% | 4.7% | 18.1% | 32.3% | 3.1% |
| edn | 449 | 639 | 15.1% | 64.1% | 12.0% | 4.2% | 6.9% | 12.7% | 0.0% |
| huffbench | 681 | 1,011 | 22.9% | 67.1% | 16.2% | 2.1% | 0.9% | 13.5% | 0.3% |
| matmult-int | 496 | 952 | 24.6% | 57.9% | 10.9% | 13.7% | 6.9% | 10.7% | 0.0% |
| md5sum | 391 | 534 | 20.1% | 60.1% | 17.1% | 6.1% | 4.1% | 12.0% | 0.5% |
| nettle-aes | 537 | 751 | 21.5% | 65.9% | 12.5% | 4.7% | 5.2% | 8.9% | 2.8% |
| nettle-sha256 | 970 | 1,106 | 18.2% | 82.8% | 6.2% | 6.9% | 0.0% | 4.1% | 0.0% |
| nsichneu | 1,166 | 1,278 | 11.9% | 75.8% | 18.7% | 2.2% | 1.2% | 2.1% | 0.0% |
| picojpeg | 4,511 | 6,602 | 23.8% | 61.8% | 12.7% | 10.4% | 3.5% | 9.7% | 1.9% |
| qrduino | 2,272 | 3,368 | 20.0% | 57.1% | 13.7% | 9.5% | 4.5% | 11.9% | 3.2% |
| sglib-combined | 1,030 | 1,564 | 25.5% | 56.1% | 16.9% | 14.9% | 0.3% | 11.3% | 0.6% |
| slre | 756 | 1,295 | 27.8% | 49.1% | 27.1% | 6.1% | 1.5% | 13.8% | 2.5% |
| statemate | 1,213 | 2,459 | 46.3% | 22.4% | 16.7% | 52.9% | 5.0% | 3.0% | 0.0% |
| tarfind | 192 | 303 | 20.8% | 45.8% | 15.1% | 8.9% | 10.4% | 19.3% | 0.5% |
| ud | 319 | 643 | 30.1% | 56.7% | 9.7% | 17.2% | 7.2% | 9.1% | 0.0% |
| wikisort | 1,097 | 2,296 | 28.1% | 66.2% | 17.7% | 1.7% | 0.1% | 13.8% | 0.5% |
| xgboost | 74 | 129 | 24.9% | 16.2% | 18.9% | 21.6% | 12.2% | 20.3% | 10.8% |
| **pooled** | 16,542 | 25,672 | 23.1% | 59.1% | 14.6% | 11.5% | 3.3% | 9.9% | 1.6% |

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
| **pooled** | 105,238,408 | 0.70 | 20.1 | 1 | 33 | 46% | 10.5% | 11.8 | 47% | 4.2% | 0.39 |

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

The quantile tables separate the field from the tail.  As written, three
quarters of the executed jumps carry a trigger distance of at most 12 and
90% at most 27, well inside the 6-bit near field; the top percent is the
far form, loop-exit jumps issued at the head of a long body and
triggering at its end (depthconv 547, matmult-int 854, edn 1,425,
nettle-sha256 2,180).  As executed the quartiles are 0, 5 and 22 and the
90th percentile 40, the same shape stretched by callees: a jump issued
before a call and triggering after the return carries the whole callee,
which is where the maxima of up to 1.5 million instructions (qrduino,
huffbench, nettle-aes, slre, xgboost) come from and why huffbench,
qrduino, slre and wikisort have an executed mean of 34 to 54 against a
written mean below 8.

| benchmark | jmp per 1000 | taken | overridden | trigger field mean | issue→trigger mean | at 0 | ≥ 32 | calls per 1000 | call/ret trigger mean | max depth |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 19.9 | 70.1% | 0.0% | 39.8 | 52.9 | 0% | 57% | 0.0 | 0.00 | 3 |
| crc32 | 21.8 | 99.6% | 0.0% | 13.9 | 40.0 | 0% | 100% | 21.7 | 0.00 | 4 |
| depthconv | 1.8 | 88.9% | 0.0% | 486.5 | 1098.8 | 0% | 100% | 0.2 | 0.00 | 4 |
| edn | 14.4 | 93.0% | 0.1% | 56.8 | 55.5 | 1% | 20% | 0.0 | 0.00 | 4 |
| huffbench | 40.6 | 54.1% | 0.3% | 7.5 | 54.0 | 16% | 1% | 0.0 | 0.00 | 5 |
| matmult-int | 2.4 | 87.4% | 0.4% | 378.3 | 350.1 | 4% | 86% | 0.0 | 0.00 | 4 |
| md5sum | 30.8 | 47.8% | 0.0% | 19.7 | 11.7 | 37% | 15% | 0.0 | 0.00 | 5 |
| nettle-aes | 7.0 | 69.6% | 0.0% | 126.1 | 354.5 | 10% | 80% | 0.0 | 0.00 | 4 |
| nettle-sha256 | 2.9 | 67.9% | 1.8% | 327.4 | 805.3 | 18% | 68% | 0.2 | 0.00 | 4 |
| nsichneu | 126.0 | 20.4% | 0.2% | 0.0 | 0.0 | 99% | 0% | 0.0 | 0.00 | 3 |
| picojpeg | 22.0 | 62.8% | 1.2% | 44.5 | 44.2 | 17% | 26% | 0.7 | 0.00 | 7 |
| qrduino | 47.6 | 44.3% | 2.1% | 5.8 | 54.2 | 23% | 3% | 0.0 | 0.00 | 6 |
| sglib-combined | 60.5 | 42.4% | 6.8% | 5.3 | 6.1 | 36% | 2% | 2.8 | 0.00 | 12 |
| slre | 69.3 | 46.1% | 4.8% | 2.3 | 34.0 | 40% | 1% | 1.6 | 0.00 | 8 |
| statemate | 24.0 | 59.2% | 1.4% | 25.7 | 90.6 | 71% | 14% | 2.0 | 0.00 | 5 |
| tarfind | 31.1 | 58.5% | 6.2% | 14.9 | 31.7 | 18% | 54% | 10.7 | 0.00 | 4 |
| ud | 34.1 | 52.2% | 8.2% | 16.8 | 21.3 | 0% | 9% | 5.3 | 0.00 | 4 |
| wikisort | 40.1 | 48.9% | 0.1% | 6.5 | 52.6 | 37% | 9% | 4.5 | 0.00 | 6 |
| xgboost | 27.1 | 77.7% | 0.1% | 6.5 | 7.8 | 0% | 0% | 0.0 | 0.00 | 4 |
| **pooled** | 32.4 | 50.1% | 2.4% | 16.8 | 53.6 | 27% | 18% | 2.3 | 0.00 | 12 |

#### Jump distance as written (trigger field)

| benchmark | mean | p10 | p25 | median | p75 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 39.8 | 1 | 1 | 2 | 90 | 90 | 91 | 153 |
| crc32 | 13.9 | 14 | 14 | 14 | 14 | 14 | 14 | 21 |
| depthconv | 486.5 | 3 | 547 | 547 | 547 | 547 | 547 | 547 |
| edn | 56.8 | 30 | 30 | 30 | 30 | 199 | 199 | 1425 |
| huffbench | 7.5 | 0 | 0 | 4 | 12 | 15 | 33 | 183 |
| matmult-int | 378.3 | 4 | 45 | 45 | 854 | 854 | 854 | 854 |
| md5sum | 19.7 | 0 | 0 | 6 | 35 | 60 | 60 | 116 |
| nettle-aes | 126.1 | 0 | 12 | 197 | 198 | 198 | 257 | 257 |
| nettle-sha256 | 327.4 | 0 | 3 | 34 | 116 | 2180 | 2180 | 2180 |
| nsichneu | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 74 |
| picojpeg | 44.5 | 0 | 3 | 9 | 24 | 273 | 334 | 335 |
| qrduino | 5.8 | 0 | 1 | 2 | 7 | 14 | 51 | 915 |
| sglib-combined | 5.3 | 0 | 0 | 1 | 6 | 17 | 38 | 183 |
| slre | 2.3 | 0 | 0 | 0 | 2 | 8 | 25 | 95 |
| statemate | 25.7 | 0 | 0 | 0 | 7 | 28 | 695 | 695 |
| tarfind | 14.9 | 0 | 2 | 10 | 27 | 27 | 116 | 116 |
| ud | 16.8 | 5 | 10 | 11 | 12 | 15 | 367 | 458 |
| wikisort | 6.5 | 0 | 0 | 2 | 8 | 18 | 45 | 112 |
| xgboost | 6.5 | 3 | 3 | 3 | 3 | 19 | 19 | 28 |
| **pooled** | 16.8 | 0 | 0 | 2 | 12 | 27 | 280 | 2180 |

#### Jump distance as executed (issue to trigger)

| benchmark | mean | p10 | p25 | median | p75 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 52.9 | 1 | 1 | 90 | 90 | 90 | 91 | 153 |
| crc32 | 40.0 | 40 | 40 | 40 | 40 | 40 | 40 | 40 |
| depthconv | 1098.8 | 547 | 547 | 547 | 547 | 4964 | 4964 | 4964 |
| edn | 55.5 | 30 | 30 | 30 | 30 | 183 | 199 | 6173 |
| huffbench | 54.0 | 0 | 4 | 4 | 15 | 15 | 41 | 977709 |
| matmult-int | 350.1 | 4 | 45 | 45 | 854 | 854 | 854 | 854 |
| md5sum | 11.7 | 0 | 0 | 9 | 11 | 35 | 60 | 3317 |
| nettle-aes | 354.5 | 0 | 88 | 197 | 198 | 198 | 257 | 108187 |
| nettle-sha256 | 805.3 | 0 | 6 | 34 | 116 | 2180 | 19223 | 19223 |
| nsichneu | 0.0 | 0 | 0 | 0 | 0 | 0 | 0 | 74 |
| picojpeg | 44.2 | 0 | 3 | 8 | 42 | 273 | 334 | 1005 |
| qrduino | 54.2 | 0 | 1 | 2 | 7 | 18 | 51 | 1531667 |
| sglib-combined | 6.1 | 0 | 0 | 2 | 7 | 17 | 38 | 1409 |
| slre | 34.0 | 0 | 0 | 1 | 2 | 8 | 32 | 78513 |
| statemate | 90.6 | 0 | 0 | 0 | 4 | 88 | 2459 | 2459 |
| tarfind | 31.7 | 0 | 2 | 53 | 53 | 53 | 53 | 116 |
| ud | 21.3 | 5 | 11 | 11 | 15 | 22 | 535 | 535 |
| wikisort | 52.6 | 0 | 0 | 5 | 6 | 19 | 301 | 34337 |
| xgboost | 7.8 | 3 | 3 | 3 | 19 | 19 | 19 | 66281 |
| **pooled** | 53.6 | 0 | 0 | 5 | 22 | 40 | 547 | 1531667 |

## Memory traffic

Loads and stores per 1,000 executed instructions, the bytes each one
moves, the share that goes to the private frame slots rather than to an
address, and the distance from a memory load to the first use of its
value (frame-slot loads are built in front of their consumer by design and
are left out).  The rv32imac columns count its loads and stores the same
way; what they access is broken down in the last table of this section.

In absolute terms Scry executes 16.7 million loads against 9.8 million
on rv32imac and 4.9 against 3.6 million stores.  41% of the Scry loads
and 37% of the stores are frame slots (6.8 and 1.8 million): without
registers, a value that is live across a block boundary or a call and
cannot stay in flight goes through the frame, and 64-bit values go as a
whole (aha-mont64: 96% of its loads are frame slots of 8 bytes).  The
addressed loads alone, 9.9 million, equal the rv32imac total, which
also contains its stack traffic.  Loads move 3.4 bytes and stores
3.3 bytes on average; the runtime's eight-byte copy loops lift wikisort
and aha-mont64 above 6.

The load-to-use distance has a mean of 48 and a median of 4.  As
written, the output reference of a load runs up against its 5-bit field:
half of the loads deliver within one instruction, but the 90th percentile
is 26 and the maximum 30, and in the loop kernels of depthconv, md5sum,
nettle-aes, nettle-sha256 and matmult-int the median reference is 24 to
26, i.e. the scheduler issues the load as early as the field allows.  As
executed, the distance follows the value through the echo and dup hops
that carry it to its consumer and through any callee in between, so the
upper quantiles stretch to 142 (p90) and 642 (p99): the loop bodies of
matmult-int, depthconv and nettle-sha256 (median 87 to 185 instructions)
and the 64-bit arithmetic of aha-mont64 (p75 of 173 for a written 26).

Loads and stores are 24% of the rv32imac instructions (18% loads, 6.5%
stores), which is ordinary for these kernels: Embench works on arrays,
tables and global state, and a register machine reaches memory with one
instruction per element.  The last table (`scripts/rv32_memory.py`, from
Spike's commit log and the disassembly; raw data `perf-rv32-memory.json`)
classifies every one by the region of its address and by how the address
is formed.  77% are computed-address accesses to the programs' own data:
global arrays and state in .data/.bss (56% of all loads and stores: the
state flags of nsichneu and statemate, the matrices of matmult-int, the
image of picojpeg and qrduino), constant tables in .rodata (19%: the
T-tables of nettle-aes, the model of xgboost, the filter weights of
depthconv, the crc table) and arrays that live on the stack (huffbench's
link array, sha256's message schedule, slre's capture structs).  The
compiler's own stack traffic is 17% of the loads and stores, i.e. 4% of
all instructions: callee-saved register saves and restores in prologues
and epilogues (8%; slre's recursive matcher, sglib's tree recursion and
wikisort's small functions), and sp-relative accesses to spill slots and
scalar locals (9%, 1.2 million).  The one benchmark where spilling
matters is matmult-int, whose fully unrolled inner product holds more
hoisted row and column addresses than there are registers: 29% of its
loads and stores (13% of its instructions) reload them from the frame.
In nettle-sha256 the sp-relative accesses are the constant-index
elements of the message schedule, not spills.  The 5.7% gp-relative
accesses are the small globals of crc32 (the random seed, read and
written once per byte), nsichneu and qrduino.  For comparison, Scry's
addressed loads and stores (9.9 and 3.1 million) match the rv32imac
data accesses, and its frame-slot traffic (6.8 and 1.8 million, 5.7% of
its instructions) plays the part of the register file that rv32imac
spills and saves far less often.

#### rv32imac loads and stores by target

Share of the window's instructions that are loads or stores, then the
share of those by region (constant tables in .rodata, globals in
.data/.bss, the stack) and by kind: callee-saved register saves and
restores, other sp-relative accesses (spills, scalar locals,
constant-index stack arrays), gp-relative small data, and every access
through a computed base register.

| benchmark | ld/st | rodata | global | stack | callee-save | sp spills/locals | gp | computed |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 0.2% | 0.0% | 0.0% | 100.0% | 0.2% | 99.7% | 0.0% | 0.0% |
| crc32 | 13.6% | 33.3% | 66.7% | 0.0% | 0.0% | 0.0% | 66.7% | 33.3% |
| depthconv | 22.0% | 87.4% | 8.7% | 3.8% | 3.8% | 0.0% | 0.0% | 96.2% |
| edn | 25.5% | 3.6% | 95.5% | 0.9% | 0.2% | 0.7% | 0.0% | 99.1% |
| huffbench | 23.0% | 0.2% | 9.4% | 90.4% | 0.0% | 0.0% | 0.0% | 100.0% |
| matmult-int | 45.4% | 0.0% | 70.7% | 29.3% | 0.0% | 29.3% | 0.0% | 70.7% |
| md5sum | 12.6% | 41.7% | 57.5% | 0.7% | 0.6% | 0.1% | 0.0% | 99.3% |
| nettle-aes | 24.1% | 67.4% | 32.0% | 0.7% | 0.5% | 0.1% | 0.0% | 99.3% |
| nettle-sha256 | 20.6% | 7.5% | 2.8% | 89.6% | 4.1% | 68.5% | 0.0% | 27.4% |
| nsichneu | 61.3% | 0.0% | 100.0% | 0.0% | 0.0% | 0.0% | 13.2% | 86.8% |
| picojpeg | 28.6% | 7.6% | 77.9% | 14.5% | 13.7% | 0.7% | 0.1% | 85.5% |
| qrduino | 19.1% | 0.5% | 99.4% | 0.1% | 0.1% | 0.0% | 45.6% | 54.2% |
| sglib-combined | 35.2% | 2.0% | 51.5% | 46.5% | 18.1% | 4.7% | 0.0% | 77.1% |
| slre | 34.8% | 13.7% | 2.0% | 84.3% | 55.7% | 24.0% | 0.0% | 20.3% |
| statemate | 72.1% | 0.0% | 93.8% | 6.2% | 6.2% | 0.0% | 0.0% | 93.8% |
| tarfind | 20.1% | 0.0% | 99.8% | 0.2% | 0.0% | 0.2% | 0.0% | 99.8% |
| ud | 22.7% | 0.0% | 86.6% | 13.4% | 4.2% | 1.5% | 0.0% | 94.3% |
| wikisort | 23.1% | 0.0% | 68.1% | 31.9% | 20.1% | 0.1% | 2.0% | 77.8% |
| xgboost | 24.9% | 99.1% | 0.0% | 0.9% | 0.3% | 0.3% | 0.0% | 99.4% |
| **pooled** | 24.2% | 19.2% | 55.5% | 25.4% | 8.3% | 9.2% | 5.7% | 76.8% |

| benchmark | loads per 1000 | bytes per load | from frame slots | stores per 1000 | bytes per store | to frame slots | load→use mean | rv32 loads per 1000 | rv32 stores per 1000 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 17.0 | 7.9 | 96.4% | 3.3 | 7.8 | 94.6% | 93.1 | 1.6 | 0.4 |
| crc32 | 87.0 | 4.0 | 50.1% | 21.8 | 4.0 | 0.2% | 3.0 | 90.9 | 45.5 |
| depthconv | 88.5 | 2.3 | 27.3% | 9.5 | 2.0 | 31.9% | 208.5 | 196.3 | 23.4 |
| edn | 128.5 | 2.8 | 36.7% | 7.8 | 3.2 | 11.8% | 34.3 | 230.6 | 24.3 |
| huffbench | 56.6 | 3.3 | 0.3% | 30.1 | 3.4 | 0.2% | 6.8 | 157.0 | 72.8 |
| matmult-int | 161.0 | 4.1 | 19.8% | 8.2 | 5.9 | 0.8% | 260.2 | 409.2 | 44.5 |
| md5sum | 79.9 | 4.0 | 72.2% | 52.1 | 3.7 | 83.0% | 25.8 | 85.8 | 39.8 |
| nettle-aes | 97.6 | 3.8 | 8.2% | 4.9 | 4.0 | 33.3% | 90.0 | 223.9 | 16.8 |
| nettle-sha256 | 21.1 | 3.7 | 16.0% | 10.2 | 3.3 | 28.1% | 340.6 | 118.4 | 87.5 |
| nsichneu | 400.9 | 4.0 | 50.2% | 1.0 | 4.0 | 40.1% | 1.3 | 611.0 | 1.8 |
| picojpeg | 70.4 | 2.1 | 36.7% | 55.1 | 2.0 | 26.3% | 23.2 | 148.2 | 137.5 |
| qrduino | 139.3 | 2.9 | 54.6% | 29.8 | 2.7 | 64.7% | 5.6 | 167.4 | 23.3 |
| sglib-combined | 89.8 | 3.7 | 6.7% | 33.8 | 3.7 | 16.3% | 2.5 | 244.5 | 107.6 |
| slre | 105.4 | 2.9 | 61.3% | 64.2 | 3.7 | 68.1% | 6.6 | 205.5 | 142.7 |
| statemate | 168.3 | 3.1 | 31.3% | 123.4 | 2.3 | 14.5% | 26.7 | 324.8 | 396.6 |
| tarfind | 62.9 | 4.4 | 74.4% | 47.4 | 4.3 | 17.6% | 2.7 | 46.7 | 154.1 |
| ud | 126.8 | 4.1 | 51.3% | 29.7 | 4.6 | 47.0% | 12.7 | 164.1 | 62.8 |
| wikisort | 46.7 | 6.3 | 39.0% | 29.6 | 6.8 | 34.8% | 4.5 | 128.5 | 102.4 |
| xgboost | 212.9 | 2.6 | 54.8% | 24.4 | 3.5 | 99.2% | 3.5 | 247.9 | 0.9 |
| **pooled** | 110.9 | 3.4 | 40.8% | 32.7 | 3.3 | 36.6% | 47.7 | 176.4 | 65.4 |

#### Load distance as written (output reference)

| benchmark | mean | p10 | p25 | median | p75 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 11.1 | 0 | 0 | 0 | 26 | 27 | 27 | 27 |
| crc32 | 3.0 | 2 | 2 | 2 | 4 | 4 | 4 | 4 |
| depthconv | 22.2 | 0 | 25 | 26 | 26 | 27 | 28 | 28 |
| edn | 13.3 | 0 | 5 | 11 | 25 | 27 | 29 | 29 |
| huffbench | 2.3 | 0 | 0 | 0 | 1 | 13 | 26 | 26 |
| matmult-int | 16.2 | 0 | 0 | 24 | 26 | 27 | 27 | 27 |
| md5sum | 16.9 | 0 | 0 | 26 | 26 | 26 | 26 | 26 |
| nettle-aes | 23.1 | 11 | 25 | 26 | 26 | 26 | 27 | 27 |
| nettle-sha256 | 15.4 | 0 | 0 | 25 | 25 | 26 | 27 | 27 |
| nsichneu | 0.5 | 0 | 0 | 0 | 0 | 2 | 2 | 2 |
| picojpeg | 5.8 | 0 | 0 | 0 | 9 | 25 | 27 | 27 |
| qrduino | 3.9 | 0 | 0 | 3 | 6 | 8 | 24 | 28 |
| sglib-combined | 1.1 | 0 | 0 | 0 | 1 | 4 | 14 | 26 |
| slre | 1.5 | 0 | 0 | 0 | 1 | 1 | 24 | 25 |
| statemate | 3.8 | 0 | 0 | 0 | 0 | 26 | 30 | 30 |
| tarfind | 2.5 | 0 | 0 | 4 | 4 | 4 | 4 | 14 |
| ud | 6.5 | 0 | 2 | 4 | 6 | 22 | 27 | 28 |
| wikisort | 1.1 | 0 | 0 | 0 | 1 | 2 | 26 | 26 |
| xgboost | 2.8 | 0 | 1 | 3 | 4 | 8 | 8 | 24 |
| **pooled** | 7.4 | 0 | 0 | 1 | 13 | 26 | 28 | 30 |

#### Load distance as executed (load to use)

| benchmark | mean | p10 | p25 | median | p75 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 93.1 | 0 | 0 | 55 | 173 | 275 | 275 | 275 |
| crc32 | 3.0 | 2 | 2 | 2 | 4 | 4 | 4 | 4 |
| depthconv | 208.5 | 25 | 83 | 181 | 311 | 419 | 506 | 506 |
| edn | 34.3 | 0 | 5 | 14 | 44 | 122 | 162 | 237 |
| huffbench | 6.8 | 0 | 1 | 2 | 2 | 18 | 61 | 113 |
| matmult-int | 260.2 | 0 | 0 | 185 | 489 | 657 | 818 | 944 |
| md5sum | 25.8 | 22 | 22 | 28 | 30 | 30 | 30 | 30 |
| nettle-aes | 90.0 | 12 | 34 | 92 | 142 | 175 | 192 | 203 |
| nettle-sha256 | 340.6 | 1 | 2 | 87 | 492 | 1510 | 1555 | 1555 |
| nsichneu | 1.3 | 0 | 0 | 0 | 2 | 5 | 5 | 5 |
| picojpeg | 23.2 | 0 | 1 | 9 | 29 | 59 | 204 | 423 |
| qrduino | 5.6 | 0 | 1 | 4 | 7 | 9 | 36 | 483 |
| sglib-combined | 2.5 | 0 | 0 | 1 | 3 | 6 | 15 | 113 |
| slre | 6.6 | 0 | 0 | 1 | 2 | 5 | 94 | 104 |
| statemate | 26.7 | 0 | 0 | 1 | 5 | 109 | 311 | 316 |
| tarfind | 2.7 | 0 | 1 | 4 | 4 | 4 | 4 | 15 |
| ud | 12.7 | 0 | 2 | 5 | 19 | 31 | 89 | 102 |
| wikisort | 4.5 | 0 | 1 | 1 | 4 | 12 | 41 | 41 |
| xgboost | 3.5 | 1 | 1 | 4 | 5 | 8 | 8 | 66 |
| **pooled** | 47.7 | 0 | 1 | 4 | 23 | 142 | 642 | 1555 |

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
| **pooled** | 11.9 | 113 | 15.5 | 115 |

## Reading the figures

For the ISA (geometric means over the benchmarks): the images are 2.8
times the size of rv32imac's, with 4.0 two-byte instructions per
rv32imac instruction, and 2.8 times as many are executed (1.8 times the
bytes); the difference is the dataflow and
constant instructions, 54% of the dynamic count, and the unrolled loop
bodies that carry their deliveries.  Half of the operand lifetimes fit the next
instruction and 90% fit the 5-bit reference; the remaining tenth, and
the constants (17% of the instructions, three quarters of them ALU
immediates that a register ISA folds into the instruction), are where
the encoding costs most.  Control flow is
issued on average 17 instructions ahead of its trigger, a quarter of it
with no lookahead.

For the compiler: the discard rate (4.2%), the forwards (0.39 per
instruction, the `grow` chains), the frame-slot traffic (41% of loads)
and the unused call and return trigger distance are the four figures the
backend could still move; the operand lifetimes beyond 31 and the
in-flight peaks are properties of the unrolled loops that trade
dataflow instructions for fewer jumps.
