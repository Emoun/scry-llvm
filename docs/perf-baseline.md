# Performance baseline

Executed-instruction metrics of the correctness-first backend (commit
`bad14a5`, every virtual register in a frame slot, far jumps everywhere) on
the 17 Embench-IoT benchmarks that run on scryer, measured with
`scryer-metrics` (`tools/scryer`) over the benchmark window between
`start_trigger` and `stop_trigger`.  The raw data is `perf-baseline.json`
(the format written by `scripts/run_embench.py --metrics --json`); compare a
new build against it with `scripts/compare_metrics.py`.

Columns: executed instructions; dataflow instructions (`echo`, `dup`, `nop`)
and their share; slot/memory instructions (`ld`, `ld.s`, `st`, `st.s`,
`saddr`); constants (`const`, `grow`); mean distance (instructions) between
a `jmp` and its trigger; mean `ld` output reference; mean and maximum
number of operands in flight in the current frame.

| benchmark | instructions | dataflow | DF% | mem | const | jmp dist | ld ref | in flight |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| aha-mont64 | 24,356,988 | 3,903,027 | 16.0 | 54.6% | 8.3% | 0.0 | 0.0 | 1.23 / 4 |
| crc32 | 24,912,394 | 3,484,164 | 14.0 | 47.6% | 22.4% | 0.0 | 0.0 | 0.95 / 3 |
| depthconv | 19,961,445 | 2,578,157 | 12.9 | 56.4% | 13.8% | 0.0 | 0.0 | 0.95 / 3 |
| edn | 36,452,182 | 5,152,666 | 14.1 | 59.1% | 8.4% | 0.0 | 0.0 | 0.98 / 4 |
| matmult-int | 22,864,569 | 3,532,906 | 15.5 | 53.0% | 16.7% | 0.0 | 0.0 | 1.19 / 4 |
| md5sum | 26,396,891 | 3,726,838 | 14.1 | 60.8% | 10.5% | 0.0 | 0.0 | 0.91 / 4 |
| nettle-aes | 31,484,596 | 5,491,697 | 17.4 | 54.1% | 9.7% | 0.0 | 0.0 | 1.33 / 5 |
| nettle-sha256 | 69,905,081 | 9,799,621 | 14.0 | 40.1% | 24.1% | 0.0 | 0.0 | 1.65 / 4 |
| nsichneu | 17,034,943 | 2,015,565 | 11.8 | 52.7% | 17.3% | 0.0 | 0.0 | 0.93 / 3 |
| picojpeg | 35,334,763 | 5,232,133 | 14.8 | 49.1% | 15.3% | 0.0 | 0.0 | 1.34 / 5 |
| qrduino | 35,476,913 | 5,050,333 | 14.2 | 50.4% | 14.6% | 0.0 | 0.0 | 1.26 / 4 |
| sglib-combined | 40,141,033 | 4,759,744 | 11.9 | 40.3% | 27.5% | 0.0 | 0.0 | 1.48 / 4 |
| slre | 22,370,915 | 2,920,429 | 13.1 | 45.6% | 18.8% | 0.0 | 0.0 | 1.25 / 6 |
| statemate | 25,527,859 | 3,333,343 | 13.1 | 49.3% | 25.3% | 0.0 | 0.0 | 1.03 / 4 |
| tarfind | 25,816,534 | 3,636,393 | 14.1 | 51.1% | 19.0% | 0.0 | 0.0 | 0.94 / 4 |
| ud | 64,761,665 | 6,952,588 | 10.7 | 34.9% | 33.7% | 0.0 | 0.0 | 1.65 / 3 |
| xgboost | 32,029,599 | 4,687,372 | 14.6 | 54.5% | 14.9% | 0.0 | 0.0 | 1.11 / 4 |
| **total** | 554,828,370 | 76,256,976 | 13.7 | | | | | |

For reference, the same benchmark windows execute 56,943,645 instructions
on rv32imac (Spike), a geometric-mean ratio of 9.3x (range 4.8x to 24.8x).
The jump distance is 0 because every jump of the baseline triggers at once,
and the load reference is 0 because every load delivers to the next
instruction.
