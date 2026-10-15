# scryer-metrics

A copy of the [scryer](https://github.com/Scry-arch/scryer) simulator's driver
(revision 6fc6ac9d, same `scry_sim` revision) that reports code-generation
metrics in addition to scryer's own:

* executed instructions per mnemonic, and the split into dataflow
  (`echo`, `echo.l`, `dup`, `nop`) and non-dataflow instructions;
* branch earliness: for every `jmp`, `call` and `ret`, the distance (in
  instructions) between the instruction and its trigger;
* load earliness: for every `ld`, the output reference (instructions between
  the load and the consumer of its value); stack loads count as 0;
* in-flight operands: after each step, the number of values waiting in the
  current function's operand queue and in all frames (max and mean);
* an optional measurement window between two symbols
  (`--window-start start_trigger --window-stop stop_trigger`), reported
  separately from the whole run;
* `--json <file>` writes everything as JSON for `scripts/compare_metrics.py`.
* `--trace` prints every executed instruction with its address, the nearest
  symbol, and the ready operands it receives (`--trace-limit N` stops after N
  instructions); invaluable for debugging generated code.

Build with `python3 scripts/build_scryer.py` (which builds stock scryer and
this crate) or `cargo build --release` in this directory. Usage is the same
as scryer's: `scryer-metrics prog.elf --target=scry32-unknown-none-elf`.
