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
   prologue/epilogue insertion) decides which virtual registers need a slot
   in the function's private stack frame.  A register with a single
   definition whose uses all follow it in the same block stays *in flight*
   and gets no slot (`-mllvm -scry-in-flight=false` gives every register a
   slot).  A register live across blocks (a PHI copy, a loop-carried value,
   a value used in several blocks) travels in flight over the edges when it
   is live into at most `-scry-flight-blocks` (8) blocks and the block it
   arrives at receives at most `-scry-max-edge-values` (32) such values
   (section 2.6; `-mllvm -scry-edge-flight=false` disables this); the rest
   get a slot.  Registers of the same size whose live intervals do not
   overlap share a slot (greedy interval colouring, `-mllvm
   -scry-share-slots=false` disables it); an in-flight copy of a slot
   register extends that slot's lifetime, because the lowering reads the
   source slot at the copy's uses.  The frame is laid out with the most
   accessed objects first (`orderFrameObjects`, by definition and use
   counts weighted by loop depth), where the short `ld.s`/`st.s` forms
   (31 scaled indices) and the single `saddr` reach them; when the slot
   candidates would not fit that window anyway, the cost model charges
   slot accesses extra.
   The `ARG_*` pseudos are first moved to the top of the entry block,
   because the arguments are delivered there whatever the scheduler did
   with the pseudos.  The generic frame-layout code then assigns final
   offsets to allocas, incoming stack arguments and these slots.

4. **Scheduling** (`ScryMachineScheduler`, at -O1 and above) reorders the
   pseudos of each block before the lowering.  Scry has no registers, so
   the usual goals of a scheduler do not apply; the order instead decides
   how early jumps and loads are issued and how far values travel in
   flight.  The strategy picks, among the ready instructions, first those
   that compute the block's branch condition (so the jump is issued as
   early as possible, far before its trigger), then the loads and the
   instructions computing their addresses (so the loads are issued long
   before their results are used), then the instruction whose inputs were
   produced most recently (short references, few values in flight); above
   a soft target of `-scry-inflight-target` (32) values in flight,
   consumers are preferred over producers.
   Constants and addresses are scheduled last, since the lowering rebuilds
   them in front of their consumer anyway.  The call pseudos are not
   scheduling barriers: a condition or a load is happily moved above a
   call.  `-mllvm -scry-sched=false` keeps the instruction-selection
   order.

5. **Temporal lowering** (`ScryTemporalLowering`, the very last pass) rewrites
   every pseudo instruction into real Scry instructions, block by block in
   instruction order.  A value in flight is an unbound output reference of
   the instruction that produced it; the reference is filled in when the
   consumer is emitted.  Slot values are stored right after their
   definition and loaded right before each use (once per block, kept in
   flight with a `dup`, when the block uses the value again before
   redefining it):

   ```
   ; %d = ADD_GR32 %a, %b   (a in flight, b loaded from its slot)
   ld u32 [b]          ; -> next instruction (the add)
   add Low =>3         ; inputs [a, b]; result -> the consumer 3 later
   ```

   The emitter keeps, per value, its pending outputs and the number of
   consumers still to come, and makes the operands of every consumer arrive
   in order with as few extra instructions as possible:

   * A value with several consumers is copied with `dup =>r1, =>r2` right
     after it is produced (one dup per two further consumers).
   * A reference is at most 31 (1023 for the long `echo`): an output whose
     consumer is not emitted in time is carried on by an `echo` hop.
   * A consumer's inputs are ordered by the execution time of their last
     emitters.  Operands already in order are aimed at the consumer
     directly; the remaining ones are re-emitted in operand order right
     before it (one long `echo` per run of values arriving in order, one
     two-reference `echo`, which swaps its two inputs, for a reversed pair).
   * Constants and addresses are next-only producers (`const`/`grow`,
     `saddr`, `ld.s`) built right in front of their consumer.  `const` and
     `grow` forward their ready inputs *behind* their own value, so values
     that must follow a constant are aimed at the constant chain; several
     constants are built in reverse order; a constant that cannot be glued
     (e.g. a call argument, since the call sits between) is captured by an
     `echo`.
   * Dead results are discarded by a `nop`.

   A call shifts the caller's pending operand lists by one position (the
   list meant for the trigger slot becomes the callee's arguments and, on
   return, the following list is merged in front of the return values), so
   references that cross a call are one larger than the instruction
   distance.

   Because every Scry instruction is exactly two bytes and the pass sees the
   final block layout, jump targets are computed directly; the only
   relaxation is the choice between the near and far jump forms described
   in section 2.3.

6. **MC layer**: real instructions are printed in the syntax of the reference
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
form.  Slot sharing is safe because a slot is written right after its
definition and read at each use, and the live intervals used for sharing
cover every read.

### 2.2 Constants and addresses

A constant is built with `const T, byte` followed by one `grow` per further
byte (`grow` shifts the value left by 8 and inserts the new low byte);
leading zero bytes, or sign-extension bytes when a signed constant does the
same job, are left out.  A symbol address built right in front of the
load, store or call that uses it as its address is PC-relative: a signed
address is taken relative to the consuming instruction, so `const i32,
%pcrel_b2(sym-6)`, `grow %pcrel_b1(sym-4)`, `grow %pcrel_b0(sym-2)` (three
instructions, reaching 8 MB either way) deliver `sym` to the instruction two
bytes after the last `grow`.  Any other symbol address is absolute: the
same sequence with four immediates and the relocations `%b3(sym)` to
`%b0(sym)`.

Constants and addresses are rematerialisable, so machine LICM hoists them
out of loops.  The slot allocator then compares, per loop depth, the cost
of rebuilding a hoisted constant at every use (its chain length), of a slot
(the chain and a store once, a load per use) and of carrying it in flight
across the loop's edges (a landing-pad instruction per block entered),
and picks the cheapest; the lowering rebuilds a constant that was given
neither a slot nor a landing position at each use.

A load or store whose address is `base + index * size` (`add nuw` of the
base and the index shifted by log2 of the access size, or a constant
non-negative multiple of the size) uses the indexed form `ld T [base,
index]` / `st [value, base, index]`: the simulator scales an unsigned
index by the access size, and `nuw` guarantees that the 32-bit sum does
not wrap, which is what the simulator's wider address arithmetic
computes.  The shift and the add disappear, and the base may be a
PC-relative symbol address.

### 2.3 Control flow

A Scry jump is *issued* ahead of time and only takes effect after the
instruction at its trigger address has executed, so the processor can learn
the direction of a branch long before the point where it diverges.  The
backend uses this: every jump of a block triggers after the block's last
instruction, and is issued as early as the block allows, at the block start
for an unconditional jump and right after the instruction that defines the
condition for a conditional one (at this stage the condition is loaded back
from its slot; the jump cannot be issued before the slot is written).

`jmp t, trig` takes a 7-bit signed target immediate and a 6-bit trigger
offset.  A target `t <= 0` is relative to the jmp itself (backward) and the
jump is taken when its condition operand is non-zero; `t > 0` is relative to
the trigger (target = trigger + t + 1) and taken when the condition is zero.
Without an operand the jump is unconditional.  This is the *near* form, one
instruction, usable when the target is within 64 instructions and the
trigger within 63:

```
ld u8 [c]          ; condition -> jmp
eq =>0             ; only when the polarity does not match the direction
jmp -22, 4         ; backward 22 instructions, triggers 4 instructions later
```

When the target or the trigger is out of range the two-operand form is
used: the target is an `i8`, `i16` or `i32` operand produced by a
`const`/`grow` chain glued before the jmp, and the two immediates together
hold a 13-bit trigger offset:

```
ld u8 [c]          ; condition
echo =>2, =>2      ; past the chain, to the jmp
const i16, hi
grow lo            ; target, relative as above
jmp 0, 9           ; [condition, target]; triggers 9 instructions later
```

An unconditional far jump has no condition to load; a `const u8` with the
polarity value (1 backward, 0 forward) is emitted *after* the target chain
(a `const` forwards its ready inputs behind its own value, which puts the
polarity first).  The form of each jump starts as near and is widened by a
relaxation loop over the function layout until every target fits.

`BRCOND %c, pol, T` branches to T when `c != 0` (`pol = 1`) or `c == 0`
(`pol = 0`); `reverseBranchCondition` flips `pol`, so branch folding and
block placement can pick either layout.  A jump whose polarity does not
match its direction negates the condition with one `eq`.  When both
successors of a block need a jump, an unconditional jump to one of them is
issued first and a conditional jump to the other, with the same trigger,
overrides it when taken (the last jump issued for a trigger address wins);
the conditional jump is given the target whose direction matches its
polarity so that no `eq` is needed.  Triggers are per call frame in the
simulator, so a jump window may span calls, including recursive ones.

### 2.4 Function entry and exit

The function's private frame starts with the incoming stack-argument area
(reserved by the caller), followed by locals and spill slots.  The prologue
reserves the remainder with `rsrv N, Private` (powers of two) after
forwarding the incoming argument list over the reserves with a long `echo`,
then spreads the up-to-four operand-passed arguments with `echo`; the
references of the spread are bound to the arguments' consumers (or slot
stores) like any other value in flight, and unused arguments go to a `nop`:

```
echo =>R                 ; echo.l: forward all args past the R reserves
rsrv 32, Private
echo =>C0, =>C1, =>      ; arg0 -> its consumer, arg1 -> its consumer, rest -> next
echo =>C2, =>C3          ; arg2, arg3 likewise
```

A single argument without reserves lands on the first instruction, which is
a long `echo` that carries it to its consumer.  A return aims the return
values past the `ret 0` trigger, frees the private reservation with the
*shared* form of `free` (the simulator only returns shared bytes to its
free pool) and executes `ret 0`.

### 2.5 Calls

Operand-passed arguments are aimed at the instruction after the call (the
trigger slot), past the callee address chain and the call.  The callee
address is the call's own first input (a glued PC-relative `const`/`grow`
chain for a symbol, or a value in flight for an indirect call).  The return value
arrives at the trigger slot, where a long `echo` (or `dup`) captures it for
its consumers, or a `nop` discards it.  Stack-passed arguments are written
to the outgoing area, which is reserved in the shared frame before the call
(`rsrv N`) and freed after it (`free N`); its address is the frame base
plus the private frame size.

### 2.6 Values in flight across block edges

References are temporal, so a value in flight when a block's jump triggers
arrives at a known position of the successor: the *p*-th instruction of
the successor receives what was aimed at "trigger + 1 + p".  Every block
therefore has a *landing pad*: one instruction per position, which
receives the value delivered to that position and re-aims it at its
consumers in the block (or, for a value this block only passes on, at the
delivery for its own successors).  A pad is a long `echo` (reach 1023, so
no hop is needed however long the block), or a `nop` when this block does
not need the value (it is needed by another successor of the delivering
block) or when nothing arrives at the position.

A block with several successors has one in-flight set at its trigger, so
it delivers the union of its successors' live-ins, and a block receives
the union of what its predecessors deliver.  Landing positions are
assigned function-wide on the final layout: two registers delivered by
the same block get different positions (greedy colouring in register
order).  Registers arriving at a block from different predecessors may
share a position, because the block needs at most one of them (a register
it needs comes from every predecessor, together with everything else
those predecessors deliver) and its pad at that position echoes the
needed one or discards whatever arrives.  At the end of a block the
emitter aims each delivered value at
its position past the block's last instruction, with the usual hops and
ordering fixups; the jump trigger is that last instruction, so the values
are in flight when the jump takes effect, and a loop-carried value never
touches memory.

Which registers travel this way is decided before the frame is laid out
(they get no slot): a register live into at most `-scry-flight-blocks`
blocks whose pads (one per block entered, plus a share of the delivery
hops, weighted by `-scry-flight-cost` percent) cost less than its slot
traffic (a store per definition and a load per use not served in flight
after a definition in the same block), subject to `-scry-max-edge-values`
per block.

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
* Code is still a few times larger than for a register machine (on the
  Embench window about 1.8 instructions per rv32imac instruction, of which
  roughly 40% are dataflow instructions).  The
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
* Pending jump triggers are kept per call frame and a jump that would skip
  over another pending trigger is an exception; the backend only ever has
  the jumps of one block pending, all with the same trigger.
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
