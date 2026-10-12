# Scry LLVM backend: design notes

This document describes how the backend maps LLVM's register-based code
generation model onto the register-less Scry ISA, the C ABI it implements,
and the layout of the repository.  It assumes familiarity with the Scry ISA
specification (`docs/scry-spec.pdf` or the Scry-arch/specification
repository) and with the scryer simulator, which is the reference for
behaviour wherever the specification is silent or differs.

## 1. The problem

Scry has no registers.  An instruction produces outputs that are delivered
to a later instruction identified by a *forward-temporal reference*: a count
of executed instructions (0 = the next instruction executed).  An
instruction's inputs are whatever earlier instructions aimed at it, ordered
by producer execution.  An instruction consumes at most four inputs
(the simulator keeps lists of four); anything else is dropped.

LLVM's machine IR, in contrast, is register based: instruction selection
produces instructions whose operands are virtual registers, and register
allocation maps those to physical registers.  There is no sensible physical
register file for Scry, so the backend keeps values in memory instead.

## 2. Overall scheme

1. **Instruction selection** (SelectionDAG) produces *virtual* pseudo
   instructions such as `ADD_GR32 %d, %a, %b`, `LD_32 %d, %addr`,
   `BRCOND %c, bb`, `CALL_32 %r, @f, %x, %y` operating on virtual
   registers of the classes `GR8`, `GR16`, `GR32`, `GR64` (i1 is promoted to
   i8).  i64 is a legal type because the ISA handles 8-byte scalars
   natively.  No floating-point types are legal: all FP operations become
   compiler-rt soft-float calls.

2. **Register allocation never runs.**  Like the WebAssembly backend, the
   pass pipeline overrides `createTargetRegisterAllocator` to return
   nothing and disables the passes that require physical registers.  PHI
   elimination and the register coalescer still run.

3. **Slot allocation** (`ScryArgHoisting` + `ScrySlotAllocation`, before
   prologue/epilogue insertion) gives every virtual register a spill slot in
   the function's private stack frame.  Registers of the same size whose
   live intervals do not overlap share a slot (greedy interval colouring,
   `-mllvm -scry-share-slots=false` disables it).  The `ARG_*` pseudos are
   first moved to the top of the entry block, because the prologue stores
   the arguments there whatever the scheduler did with the pseudos.  The
   generic frame-layout code then assigns final offsets to allocas, incoming
   stack arguments and these slots.

4. **Temporal lowering** (`ScryTemporalLowering`, the very last pass) rewrites
   every pseudo instruction into real Scry instructions.  Every operand is
   loaded from its slot right before use and every result is stored into its
   slot right after it is produced, so values are in flight for only a few
   instructions and all references are resolved locally:

   ```
   ; %d = ADD_GR32 %a, %b
   ld u32 [a]          ; -> next instruction
   echo =>1, =>1       ; forward a past the next load, to the add
   ld u32 [b]          ; -> next instruction (the add)
   add Low =>0         ; inputs [a, b]; result -> next
   st [d]
   ```

   Because every Scry instruction is exactly two bytes and the pass sees the
   final block layout, branch displacements are computed directly; no
   relaxation or label fixups are needed inside functions.

5. **MC layer**: real instructions are printed in the syntax of the reference
   `scry_asm` assembler and encoded to ELF objects (`e_machine` 0x264).  The
   only relocations in code are byte-wise absolute addresses in `const`/`grow`
   immediates (`R_SCRY_IMM8_B0..B3`); data uses `R_SCRY_8/16/32/64`.  lld
   links the objects with image base 0x100000, clear of the simulator's stack
   at 0x10000.

### 2.1 Slot access forms

Stack loads/stores (`ld T [idx]`, `st [idx]`) have a 5-bit index scaled by
the type, so slots within 31 scaled units of the frame base use one
instruction.  Farther slots use the address of the frame base (`saddr`)
plus a constant and an absolute `ld`/`st`.  Slot allocation places 8-byte
slots first, then 4, 2 and 1, so most slots are reachable with the short
form.  Slot sharing is safe because every expansion loads all of its
inputs before it stores its result.

### 2.2 Constants and addresses

A constant is built with `const T, byte` followed by one `grow` per further
byte (`grow` shifts the value left by 8 and inserts the new low byte).
Symbol addresses use the same sequence with relocations
`%b3(sym)`, `%b2(sym)`, `%b1(sym)`, `%b0(sym)` on the four immediates.

### 2.3 Control flow

`jmp` takes a 7-bit relative target immediate and a 6-bit trigger offset,
and its condition polarity depends on the direction: forward jumps are taken
when the condition is zero, backward jumps when it is non-zero.  To keep
things uniform the backend always emits a *far jump* sequence with a
computed 16-bit (or 32-bit for huge functions) displacement in an operand:

```
const u8, c        ; c = 0 for forward, 1 for backward (polarity)
echo =>2, =>2      ; forward c to the jmp
const i16, hi
grow lo            ; displacement in instructions
jmp 0, 0           ; two-operand form: [condition, target]; triggers at once
```

A conditional branch `BRCOND %c, T` becomes "load c; echo to J; J: jmp
S, 0 (skip S instructions when c == 0); far jump to T".  When T is the
layout successor and an unconditional branch follows, the condition is
negated with `eq` and the following far jump is skipped instead.

### 2.4 Function entry and exit

The function's private frame starts with the incoming stack-argument area
(reserved by the caller), followed by locals and spill slots.  The prologue
reserves the remainder with `rsrv N, Private` (powers of two) after
forwarding the incoming argument list over the reserves with a long `echo`,
then spreads the up-to-four operand-passed arguments to their slots:

```
echo =>R                 ; echo.l: forward all args past the R reserves
rsrv 32, Private
echo =>D0, =>D1, =>      ; arg0 -> D0, arg1 -> D1, rest -> next
echo =>D2, =>D3          ; arg2 -> D2, arg3 -> D3
st [s0]  st [s1]  st [s2]  st [s3]
```

A return loads the return values, aims them at the slot after `ret 0`,
frees the private reservation with the *shared* form of `free` (the
simulator only returns shared bytes to its free pool) and executes `ret 0`.

### 2.5 Calls

Operand-passed arguments are aimed at the instruction after the call (the
trigger slot).  The callee address is the call's own first input.  The
return value arrives at the trigger slot, where it is stored (or discarded
with `nop`).  Stack-passed arguments are written to the outgoing area, which
is reserved in the shared frame before the call (`rsrv N`) and freed after
it (`free N`); its address is the frame base plus the private frame size.

## 3. C ABI

* Types: `char` 8 bits (signed), `short` 16, `int` 32, `long` 32,
  `long long` 64, pointers 32, `float` 32, `double`/`long double` 64 (IEEE,
  soft float).  Stack frames and `max_align_t` are 16-byte aligned.
  Data layout: `e-m:e-p:32:32-i64:64-i128:128-n8:16:32:64-S128`.
* All values are tagged unsigned in flight; signed operations re-tag their
  inputs with `cast` where it matters (signed compares, arithmetic shifts,
  sign extension).  Signed division uses compiler-rt (`__divsi3` etc.)
  because the ISA's division is Euclidean, not truncating.
* Argument passing: the first four scalar arguments travel as operands of
  the call trigger in order; further named scalar arguments go on the
  stack (the outgoing area of the caller's shared frame), each at the next
  offset aligned to its size.  Integers narrower than 32 bits are passed
  as 32-bit values (sign- or zero-extended according to the parameter's
  signedness, otherwise with unspecified upper bits) and truncated by the
  callee, which matches the C integer promotions and keeps the compiler's
  own library calls (e.g. `memset` with an `i8` value) compatible with
  their C implementations.  Return values narrower than 32 bits are
  widened the same way (LLVM itself only widens `zeroext`/`signext`
  returns on the callee side, so the backend does it on both sides).  Aggregates are never passed or returned by
  value: clang passes a pointer to a caller-made copy and returns aggregates
  through an `sret` pointer.  A single scalar result is returned through
  the return trigger.
* Variadic calls: the caller writes the variadic arguments into a 16-byte
  aligned buffer in its own frame (each at the next offset aligned to its
  size) and passes the buffer's address as a hidden trailing argument,
  after all named ones.  `va_list` is a `char *` into that buffer; the
  callee reads it with `va_start`/`va_arg`.  This is the WebAssembly scheme
  and avoids the callee having to know where the caller's stack arguments
  live.
* Zero-initialised globals are placed in `.data`, not `.bss` (the driver
  adds `-fno-zero-initialized-in-bss`): the scryer ELF loader marks bytes
  that are not present in the file as uninitialised and traps on reads.
* The ELF entry is `_start` (in `crt0`), which calls `main` and returns its
  value; the simulator uses the low byte as the exit code.  `exit()` cannot
  unwind the Scry call stack and therefore records the status and traps.

## 4. Limitations (current)

* Dynamic stack allocation (variable-length arrays, `alloca`) and
  `setjmp`/`longjmp` are not supported.
* Indirect branches (`indirectbr`) are converted to switches; jump tables
  are not used.
* No inline assembly, no assembler for `.s` input (object files are produced
  directly from the compiler), no disassembler.
* `scry64` is registered in the triple parser but no code generation exists
  for it yet.
* Code is large: roughly 5-10 real instructions per IR operation.  The
  scryer simulator's stack is fixed at 4 KB, which bounds the call depth and
  frame sizes of programs that can run.

## 5. Notes on the scryer simulator

Observations made while bringing the backend up, which matter for anyone
running the generated code (revision 6fc6ac9d of scryer):

* The stack is a fixed 4 KB buffer (`stack_buffer = 1 << 12` in
  `src/lib.rs`); exhausting it panics with "TODO: Reserve inadequate
  buffer" and the exit code is 101.  Deep recursion or large frames need a
  larger buffer in the simulator.
* The ELF loader (`add_block_zeroed`) marks bytes that have no file
  contents as *uninitialised* and reading them panics (`todo!()` in
  `perform_load`), hence the `.data` placement of zero-initialised globals.
  Uninitialised stack memory is the same, which is why every slot is
  written before it is read.
* `rsrv` and `free` panic if they receive an operand; the prologue therefore
  starts with `nop` when the function expects no operands, so that a caller
  passing extra arguments (e.g. `_start` calling `main(0, NULL)`) is safe.
* `free N, Private` does not return the bytes to the free pool; the backend
  frees with shared `free N` instead.
* The two-operand `jmp` is treated as relative, like the one-operand form,
  although the specification calls it absolute; the backend emits relative
  displacements.
* `pick` with an immediate (`PickI`) is unimplemented; the backend only
  uses the register form.
* `--target=scry32-unknown-none-elf` must be given explicitly; target
  detection from the ELF header is unimplemented.

## 6. Repository layout

```
llvm/            overlay copied into third_party/llvm-project (the backend,
                 clang target, lld port, tests)
patches/         small modifications to upstream LLVM files
runtime/         crt0, libc, libm, list of compiler-rt builtins
scripts/         cross-platform Python scripts (setup, build, test, embench)
tests/c/         C programs with expected exit codes, run on scryer
embench/         Embench-IoT board support for Scry
docs/            this document and the ISA specification
```
