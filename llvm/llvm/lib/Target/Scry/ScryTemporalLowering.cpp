//===-- ScryTemporalLowering.cpp - Lower virtual code to real Scry code ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the heart of the Scry backend.  It runs last, after the frame layout
// is final, and rewrites every "virtual" pseudo instruction (operating on
// virtual registers) into a sequence of real Scry instructions.
//
// Scry has no registers: an instruction's outputs are delivered to a later
// instruction identified by a forward-temporal reference (0 = the next
// instruction executed, 1 = the one after, ...), and an instruction's inputs
// are whatever earlier instructions aimed at it, in producer order.
//
// The scheme used here is deliberately simple ("correctness first"): every
// virtual register owns a slot in the private stack frame.  An operation
//
//     %d = ADD %a, %b
//
// becomes
//
//     ld u32 [a]        ; load slot of %a, output to next instruction
//     echo =>1, =>1     ; forward it past the next load to the add
//     ld u32 [b]        ; load slot of %b, output to the add
//     add Low =>0       ; inputs [a, b]; output to next instruction
//     st [d]            ; store the result in the slot of %d
//
// Values therefore never stay in flight for more than a handful of
// instructions, every reference is resolved locally, and control flow needs no
// in-flight values at all.
//
// Branches: a Scry jmp is issued ahead of time and only takes effect after the
// instruction at its trigger address.  The jumps of a block are issued as early
// as the block allows (right after the condition is computed, or at the block
// start for unconditional jumps) and trigger after the block's last
// instruction, so the processor learns the direction well in advance.  A near
// jump (target within 64 instructions, trigger within 63) is a single
// instruction; otherwise the target is an immediate operand produced by a
// glued const/grow chain and the trigger offset is 13 bits.  The form of each
// jump is chosen by relaxation once the layout is known (all Scry
// instructions are exactly two bytes, so the layout is exact).  When both
// successors need a jump, an unconditional jump is issued first and a
// conditional one with the same trigger overrides it when taken.
//
// Function entry: the arguments delivered to the first instruction are spread
// to their slots with echo/dup, after the private frame has been reserved:
//
//     echo =>R          ; (echo.l) forward all args over the R reserves
//     rsrv N, Private   ; reserve the local frame (powers of two)
//     echo =>D0, =>D1, =>  ; arg0 -> slot store 0, arg1 -> slot store 1, rest on
//     echo =>D2, =>D3   ; arg2 -> slot store 2, arg3 -> slot store 3
//     st [s0] ...
//
// Calls: arguments are aimed at the instruction after the call (the trigger
// slot), the callee address is the call's own input, and the return value
// arrives at that same slot, where it is stored:
//
//     ld u32 [x]  echo =>K    ...   const u32,%b3(f)  grow %b2(f) ... call 0
//     st [r]
//
// Returns: the values are aimed at the instruction after "ret 0", preceded by
// freeing the local frame.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/ScryMCTargetDesc.h"
#include "Scry.h"
#include "ScryInstrInfo.h"
#include "ScryMachineFunctionInfo.h"
#include "ScrySubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "scry-temporal"

STATISTIC(NumNearJumps, "Number of single-instruction (near) jumps");
STATISTIC(NumFarJumps, "Number of two-operand (far) jumps");
STATISTIC(NumJumpNegations, "Number of jump conditions negated with eq");
STATISTIC(JumpTrigDistance, "Sum of jump issue-to-trigger distances");

namespace {

/// A real instruction waiting to be materialised.
struct Item {
  unsigned Opc = 0;
  SmallVector<int64_t, 2> Imms;

  // Pending forward references: Imms[RefOp] := Target - index - 1.
  int RefOp = -1;
  int RefTarget = -1;
  int Ref2Op = -1;
  int Ref2Target = -1;

  // Symbol operand of CONST/GROW (ADDR_SYM expansion).
  const GlobalValue *GV = nullptr;
  const char *ExtSym = nullptr;
  const BlockAddress *BA = nullptr;
  int CPIdx = -1;
  int64_t SymOffset = 0;
  unsigned SymByte = 0;

  // Late-bound trigger of a jmp: Imms := trigger offset TrigTarget - index,
  // split into the two immediates for the far form.
  int TrigTarget = -1;

  // Jump pieces, resolved once the block layout is known.  The jmp item has
  // JumpTarget set; the const/grow items producing its operands refer to it
  // through JumpItem.
  enum DeferredKind { None, JumpPolarity, JumpByte };
  DeferredKind Deferred = None;
  MachineBasicBlock *JumpTarget = nullptr;
  const MachineInstr *JumpKey = nullptr; // terminator this jmp implements
  bool FarJump = false;                  // two-operand form
  int64_t TargetValue = 0;               // relative target once laid out
  int JumpItem = -1;
  unsigned JumpByteIdx = 0;

  std::string Comment;
};

struct BlockSeq {
  MachineBasicBlock *MBB;
  std::vector<Item> Items;
  unsigned Start = 0; // absolute index of the first item within the function
};

class ScryTemporalLowering final : public MachineFunctionPass {
public:
  static char ID;
  ScryTemporalLowering() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Scry temporal lowering"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  MachineFunction *MF = nullptr;
  const ScryInstrInfo *TII = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  ScryFunctionInfo *FuncInfo = nullptr;

  unsigned IncomingSize = 0; // incoming stack-argument area (bytes)
  unsigned ReserveSize = 0;  // bytes reserved by the prologue
  unsigned PrivSize = 0;     // total private frame size

  std::vector<BlockSeq> Blocks;
  BlockSeq *Cur = nullptr;

  // --- jumps ---------------------------------------------------------------
  // Forms a jump can take, from cheapest to most general; the layout
  // relaxation only ever moves a jump towards the more general forms.
  enum JumpForm { JF_Near, JF_Far8, JF_Far16, JF_Far32 };
  DenseMap<const MachineInstr *, JumpForm> JumpForms;
  DenseMap<const MachineBasicBlock *, unsigned> LayoutIndex;
  unsigned FnNearJumps = 0, FnFarJumps = 0, FnNegations = 0; // this round

  struct PlannedJump {
    const MachineInstr *Key;
    MachineBasicBlock *Target;
    Register Cond;             // invalid: unconditional
    bool TakenOnNonZero;       // with Cond: polarity of the branch
    unsigned EarliestBoundary; // first boundary at which it may be issued
  };
  bool isBackward(const MachineBasicBlock *From,
                  const MachineBasicBlock *To) const {
    return LayoutIndex.lookup(To) <= LayoutIndex.lookup(From);
  }
  void planJumps(MachineBasicBlock &MBB, SmallVectorImpl<PlannedJump> &Jumps);
  // Negating compares whose final eq is folded into the jump they feed.
  DenseSet<const MachineInstr *> FoldedNegations;
  bool jumpNeedsNegation(const MachineBasicBlock &MBB, const PlannedJump &J) const {
    return J.TakenOnNonZero != isBackward(&MBB, J.Target);
  }
  void planNegationFolds(MachineBasicBlock &MBB, ArrayRef<PlannedJump> Jumps);
  void buildJumpSeq(MachineBasicBlock &MBB, const PlannedJump &J, JumpForm Form,
                    std::vector<Item> &Seq);
  void insertItems(unsigned Pos, std::vector<Item> &Seq, const PlannedJump &J);
  void insertJumps(MachineBasicBlock &MBB, ArrayRef<unsigned> Boundaries,
                   ArrayRef<PlannedJump> Jumps);

  // --- slot helpers -------------------------------------------------------
  unsigned slotOffset(Register R) const;
  unsigned scaleOf(Register R) const;
  unsigned log2Scale(Register R) const { return Log2_32(scaleOf(R)); }
  unsigned utype(Register R) const { return Scry::unsignedTypeCode(log2Scale(R)); }
  unsigned stype(Register R) const { return Scry::signedTypeCode(log2Scale(R)); }

  // --- emission helpers ---------------------------------------------------
  int emit(unsigned Opc, std::initializer_list<int64_t> Imms = {});
  void setRef(int Idx, int TargetIdx, bool Second = false);
  int curIndex() const { return (int)Cur->Items.size(); }

  unsigned frameAddrLen(unsigned Off) const;
  void emitFrameAddr(unsigned Off);
  void emitConstBytes(unsigned TypeCode3, uint64_t Value, unsigned Scale);
  void emitLoadSlot(Register R);
  unsigned storeHelperLen(Register R) const;
  void emitStoreSlot(Register R);
  void emitFrees();
  void emitReserves(unsigned Bytes, bool Private);

  // --- expansion ----------------------------------------------------------
  void expandPrologue(MachineBasicBlock &Entry);
  void expandInstr(MachineInstr &MI, MachineBasicBlock &MBB);
  void expandBinary(MachineInstr &MI, unsigned RealOpc);
  void expandRightShift(MachineInstr &MI, bool Arithmetic);
  void expandCompare(MachineInstr &MI, unsigned First, bool Negate,
                     bool Signed);
  void expandCasts(MachineInstr &MI, ArrayRef<unsigned> TypeCodes);
  void expandCall(MachineInstr &MI);
  void expandReturn(MachineInstr &MI);

  bool expandFunction();
  void resolveAndMaterialise();
};

} // end anonymous namespace

char ScryTemporalLowering::ID = 0;

INITIALIZE_PASS(ScryTemporalLowering, DEBUG_TYPE, "Scry temporal lowering",
                false, false)

FunctionPass *llvm::createScryTemporalLoweringPass() {
  return new ScryTemporalLowering();
}

//===----------------------------------------------------------------------===//
// Slot helpers
//===----------------------------------------------------------------------===//

unsigned ScryTemporalLowering::slotOffset(Register R) const {
  int FI = FuncInfo->getSpillSlot(R);
  if (FI < 0)
    report_fatal_error("Scry: virtual register without a stack slot");
  int64_t Off = MF->getFrameInfo().getObjectOffset(FI);
  assert(Off >= 0);
  return (unsigned)Off;
}

unsigned ScryTemporalLowering::scaleOf(Register R) const {
  const TargetRegisterClass *RC = MRI->getRegClass(R);
  if (RC == &Scry::GR8RegClass)
    return 1;
  if (RC == &Scry::GR16RegClass)
    return 2;
  if (RC == &Scry::GR32RegClass)
    return 4;
  if (RC == &Scry::GR64RegClass)
    return 8;
  report_fatal_error("Scry: unexpected register class");
}

//===----------------------------------------------------------------------===//
// Emission helpers
//===----------------------------------------------------------------------===//

int ScryTemporalLowering::emit(unsigned Opc,
                               std::initializer_list<int64_t> Imms) {
  Item I;
  I.Opc = Opc;
  I.Imms.assign(Imms.begin(), Imms.end());
  Cur->Items.push_back(std::move(I));
  return (int)Cur->Items.size() - 1;
}

void ScryTemporalLowering::setRef(int Idx, int TargetIdx, bool Second) {
  Item &I = Cur->Items[Idx];
  if (Second) {
    I.Ref2Op = 1;
    I.Ref2Target = TargetIdx;
  } else {
    I.RefOp = 0;
    I.RefTarget = TargetIdx;
  }
}

/// Number of instructions emitConstBytes will emit.
static unsigned constBytesLen(uint64_t Value, unsigned Scale) {
  unsigned N = 1;
  for (unsigned i = 1; i < Scale; ++i)
    if ((Value >> (8 * i)) & 0xff)
      N = i + 1;
  return N;
}

/// Emit a constant of the given 3-bit type code; the value goes to the next
/// instruction.  Any ready inputs are forwarded behind it.
void ScryTemporalLowering::emitConstBytes(unsigned TypeCode3, uint64_t Value,
                                          unsigned Scale) {
  unsigned N = constBytesLen(Value, Scale);
  emit(Scry::CONST, {(int64_t)TypeCode3, (int64_t)((Value >> (8 * (N - 1))) & 0xff)});
  for (int i = (int)N - 2; i >= 0; --i)
    emit(Scry::GROW, {(int64_t)((Value >> (8 * i)) & 0xff)});
}

/// Can the address of frame offset Off be produced by a single saddr?
static bool saddrForm(unsigned Off, unsigned &SizeCode, unsigned &Idx) {
  for (unsigned S = 3; S != (unsigned)-1; --S) {
    unsigned Scale = 1u << S;
    if (Off % Scale == 0 && Off / Scale <= 31) {
      SizeCode = S;
      Idx = Off / Scale;
      return true;
    }
  }
  return false;
}

unsigned ScryTemporalLowering::frameAddrLen(unsigned Off) const {
  unsigned S, I;
  if (saddrForm(Off, S, I))
    return 1;
  // saddr u8 [0]; const u32 (+grows); add
  return 2 + constBytesLen(Off, 4);
}

/// Emit code producing the absolute address of private-frame offset Off,
/// delivered to the next instruction.
void ScryTemporalLowering::emitFrameAddr(unsigned Off) {
  unsigned S, I;
  if (saddrForm(Off, S, I)) {
    emit(Scry::SADDR, {(int64_t)S, (int64_t)I});
    return;
  }
  emit(Scry::SADDR, {0, 0}); // frame base -> const (forwarded behind imm)
  emitConstBytes(Scry::T_U32, Off, 4);
  emit(Scry::ADD_L, {0}); // [off, base] -> base + off
}

/// Emit code loading the slot of R; the value goes to the next instruction.
void ScryTemporalLowering::emitLoadSlot(Register R) {
  unsigned Off = slotOffset(R);
  unsigned Scale = scaleOf(R);
  if (Off % Scale == 0 && Off / Scale <= 31) {
    emit(Scry::LDS, {(int64_t)utype(R), (int64_t)(Off / Scale)});
    return;
  }
  emitFrameAddr(Off);
  emit(Scry::LD, {(int64_t)utype(R), 0});
}

unsigned ScryTemporalLowering::storeHelperLen(Register R) const {
  unsigned Off = slotOffset(R);
  unsigned Scale = scaleOf(R);
  if (Off % Scale == 0 && Off / Scale <= 31)
    return 1;
  return 2 + frameAddrLen(Off);
}

/// Emit code storing a value into the slot of R.  The value must be aimed at
/// the first instruction emitted here.
void ScryTemporalLowering::emitStoreSlot(Register R) {
  unsigned Off = slotOffset(R);
  unsigned Scale = scaleOf(R);
  if (Off % Scale == 0 && Off / Scale <= 31) {
    emit(Scry::STS, {(int64_t)(Off / Scale)});
    return;
  }
  // Forward the value over the address computation to the store.
  unsigned Len = frameAddrLen(Off);
  emit(Scry::ECHO, {(int64_t)Len, (int64_t)Len});
  emitFrameAddr(Off);
  emit(Scry::ST);
}

void ScryTemporalLowering::emitReserves(unsigned Bytes, bool Private) {
  for (unsigned K = 0; K < 16; ++K)
    if (Bytes & (1u << K))
      emit(Private ? Scry::RSRVP : Scry::RSRV, {(int64_t)K});
}

/// Free the bytes reserved by the prologue.  The shared-frame form is used
/// because the simulator only returns shared bytes to its free pool.
void ScryTemporalLowering::emitFrees() {
  for (unsigned K = 0; K < 16; ++K)
    if (ReserveSize & (1u << K))
      emit(Scry::FREE, {(int64_t)K});
}

//===----------------------------------------------------------------------===//
// Expansion of individual instructions
//===----------------------------------------------------------------------===//

void ScryTemporalLowering::expandBinary(MachineInstr &MI, unsigned RealOpc) {
  Register D = MI.getOperand(0).getReg();
  Register A = MI.getOperand(1).getReg();
  Register B = MI.getOperand(2).getReg();
  emitLoadSlot(A);
  int E = emit(Scry::ECHO, {0, 0});
  emitLoadSlot(B);
  int Op = emit(RealOpc, {0});
  setRef(E, Op);
  setRef(E, Op, true);
  emitStoreSlot(D);
}

/// Logical / arithmetic right shift: the shift amount must be a negative
/// signed value, so compute 0 - n (as i32).
void ScryTemporalLowering::expandRightShift(MachineInstr &MI,
                                            bool Arithmetic) {
  Register D = MI.getOperand(0).getReg();
  Register A = MI.getOperand(1).getReg();
  Register N = MI.getOperand(2).getReg();
  // Value operand -> shift.
  emitLoadSlot(A);
  int RouteA = Arithmetic ? emit(Scry::CAST, {(int64_t)stype(A), 0})
                          : emit(Scry::ECHO, {0, 0});
  // 0 (i32) -> sub.
  emit(Scry::CONST, {(int64_t)Scry::T_I32, 0});
  int E0 = emit(Scry::ECHO, {0, 0});
  // n (as i32) -> sub (next).
  emitLoadSlot(N);
  emit(Scry::CAST, {(int64_t)Scry::T_I32, 0});
  int Sub = emit(Scry::SUB_L, {0}); // -> shift (next)
  int Sh = emit(Scry::SH_L, {0});
  setRef(E0, Sub);
  setRef(E0, Sub, true);
  if (Arithmetic) {
    Cur->Items[RouteA].RefOp = 1;
    Cur->Items[RouteA].RefTarget = Sh;
  } else {
    setRef(RouteA, Sh);
    setRef(RouteA, Sh, true);
  }
  emitStoreSlot(D);
}

/// Comparison producing a u8 boolean: First is EQ/LT/GT, optionally negated
/// by a second eq (against the implicit 0).
void ScryTemporalLowering::expandCompare(MachineInstr &MI, unsigned First,
                                         bool Negate, bool Signed) {
  Register D = MI.getOperand(0).getReg();
  Register A = MI.getOperand(1).getReg();
  Register B = MI.getOperand(2).getReg();
  emitLoadSlot(A);
  int RouteA = Signed ? emit(Scry::CAST, {(int64_t)stype(A), 0})
                      : emit(Scry::ECHO, {0, 0});
  emitLoadSlot(B);
  if (Signed)
    emit(Scry::CAST, {(int64_t)stype(B), 0});
  int Cmp = emit(First, {0});
  if (Signed) {
    Cur->Items[RouteA].RefOp = 1;
    Cur->Items[RouteA].RefTarget = Cmp;
  } else {
    setRef(RouteA, Cmp);
    setRef(RouteA, Cmp, true);
  }
  if (Negate && !FoldedNegations.count(&MI))
    emit(Scry::EQ, {0});
  emitStoreSlot(D);
}

/// Load the source, apply a chain of casts, store the destination.
void ScryTemporalLowering::expandCasts(MachineInstr &MI,
                                       ArrayRef<unsigned> TypeCodes) {
  Register D = MI.getOperand(0).getReg();
  Register S = MI.getOperand(1).getReg();
  emitLoadSlot(S);
  for (unsigned T : TypeCodes)
    emit(Scry::CAST, {(int64_t)T, 0});
  emitStoreSlot(D);
}

void ScryTemporalLowering::expandCall(MachineInstr &MI) {
  bool HasResult = MI.getNumExplicitDefs() == 1;
  unsigned CalleeIdx = HasResult ? 1 : 0;
  const MachineOperand &CalleeMO = MI.getOperand(CalleeIdx);

  // Arguments -> the trigger slot (instruction after the call).
  SmallVector<int, 4> Echos;
  for (unsigned i = CalleeIdx + 1, e = MI.getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI.getOperand(i);
    if (!MO.isReg() || MO.isImplicit())
      continue;
    emitLoadSlot(MO.getReg());
    Echos.push_back(emit(Scry::ECHO, {0, 0}));
  }

  // Callee address -> call (next).
  if (CalleeMO.isReg()) {
    emitLoadSlot(CalleeMO.getReg());
  } else {
    for (int B = 3; B >= 0; --B) {
      int Idx = B == 3 ? emit(Scry::CONST, {(int64_t)Scry::T_U32, 0})
                       : emit(Scry::GROW, {0});
      Item &I = Cur->Items[Idx];
      I.SymByte = B;
      if (CalleeMO.isGlobal()) {
        I.GV = CalleeMO.getGlobal();
        I.SymOffset = CalleeMO.getOffset();
      } else if (CalleeMO.isSymbol()) {
        I.ExtSym = CalleeMO.getSymbolName();
        I.SymOffset = CalleeMO.getOffset();
      } else {
        report_fatal_error("Scry: unexpected callee operand");
      }
    }
  }
  int Call = emit(Scry::CALL, {0});
  int Trigger = Call + 1;
  for (int E : Echos) {
    setRef(E, Trigger);
    setRef(E, Trigger, true);
  }
  // The return value arrives at the trigger slot.
  if (HasResult)
    emitStoreSlot(MI.getOperand(0).getReg());
  else
    emit(Scry::NOP);
}

void ScryTemporalLowering::expandReturn(MachineInstr &MI) {
  SmallVector<int, 4> Echos;
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || MO.isImplicit())
      continue;
    emitLoadSlot(MO.getReg());
    Echos.push_back(emit(Scry::ECHO, {0, 0}));
  }
  emitFrees();
  int Ret = emit(Scry::RET, {0});
  for (int E : Echos) {
    setRef(E, Ret + 1);
    setRef(E, Ret + 1, true);
  }
}

void ScryTemporalLowering::expandPrologue(MachineBasicBlock &Entry) {
  unsigned NumArgs = FuncInfo->getNumArgOperands();
  unsigned NumReserves = llvm::popcount(ReserveSize);

  // Destination register of each argument delivered as an operand.
  SmallVector<Register, 4> ArgRegs(NumArgs, Register());
  for (MachineInstr &MI : Entry) {
    switch (MI.getOpcode()) {
    case Scry::ARG_8:
    case Scry::ARG_16:
    case Scry::ARG_32:
    case Scry::ARG_64: {
      unsigned Idx = MI.getOperand(1).getImm();
      if (Idx < NumArgs)
        ArgRegs[Idx] = MI.getOperand(0).getReg();
      break;
    }
    default:
      break;
    }
  }

  if (NumArgs == 0) {
    // Discard whatever a caller may have delivered (e.g. extra arguments to a
    // main(void), or simulator inputs): reserves must not receive operands and
    // other instructions would misinterpret them.
    emit(Scry::NOP);
    emitReserves(ReserveSize, /*Private=*/true);
    return;
  }
  if (NumReserves > 0)
    emit(Scry::ECHOL, {(int64_t)NumReserves});
  emitReserves(ReserveSize, /*Private=*/true);

  // Spread the argument list to the per-argument store helpers.
  SmallVector<int, 2> Spreads;
  if (NumArgs >= 2)
    Spreads.push_back(emit(NumArgs >= 3 ? Scry::ECHOS : Scry::ECHO, {0, 0}));
  if (NumArgs >= 3)
    Spreads.push_back(emit(Scry::ECHO, {0, 0}));

  SmallVector<int, 4> Dests;
  for (unsigned i = 0; i < NumArgs; ++i) {
    Dests.push_back(curIndex());
    if (ArgRegs[i].isValid() && !MRI->use_nodbg_empty(ArgRegs[i]))
      emitStoreSlot(ArgRegs[i]);
    else
      emit(Scry::NOP); // unused argument: discard
  }

  if (NumArgs >= 2) {
    setRef(Spreads[0], Dests[0]);
    setRef(Spreads[0], Dests[1], true);
  }
  if (NumArgs >= 3) {
    setRef(Spreads[1], Dests[2]);
    setRef(Spreads[1], NumArgs >= 4 ? Dests[3] : Dests[2], true);
  }
}

static std::string describe(const MachineInstr &MI) {
  std::string S;
  raw_string_ostream OS(S);
  MI.print(OS, /*IsStandalone=*/true, /*SkipOpers=*/false,
           /*SkipDebugLoc=*/true, /*AddNewLine=*/false);
  return S;
}

//===----------------------------------------------------------------------===//
// Jumps
//===----------------------------------------------------------------------===//

/// Decide which jumps the terminators of MBB need.  At most two: an
/// unconditional one (listed first, issued first) and a conditional one that
/// shares its trigger and overrides it when taken.
void ScryTemporalLowering::planJumps(MachineBasicBlock &MBB,
                                     SmallVectorImpl<PlannedJump> &Jumps) {
  MachineInstr *CondBr = nullptr, *UncondBr = nullptr;
  for (MachineInstr &MI : MBB.terminators()) {
    if (MI.getOpcode() == Scry::BR)
      UncondBr = &MI;
    else if (MI.getOpcode() == Scry::BRCOND)
      CondBr = &MI;
  }

  // Boundary index just after the instruction defining R, if it is in MBB.
  auto boundaryAfterDef = [&](Register R) -> unsigned {
    MachineInstr *Def = MRI->getVRegDef(R);
    if (!Def || Def->getParent() != &MBB)
      return 0;
    unsigned N = 0;
    for (MachineInstr &MI : MBB) {
      if (MI.isTerminator())
        break;
      ++N;
      if (&MI == Def)
        return N;
    }
    return 0;
  };
  auto cond = [&](MachineBasicBlock *Tgt, bool Pol) {
    Register C = CondBr->getOperand(0).getReg();
    Jumps.push_back({CondBr, Tgt, C, Pol, boundaryAfterDef(C)});
  };
  auto uncond = [&](MachineBasicBlock *Tgt) {
    Jumps.push_back({UncondBr, Tgt, Register(), false, 0});
  };

  MachineBasicBlock *F = UncondBr ? UncondBr->getOperand(0).getMBB() : nullptr;
  if (!CondBr) {
    if (F && !MBB.isLayoutSuccessor(F))
      uncond(F);
    return;
  }
  MachineBasicBlock *T = CondBr->getOperand(2).getMBB();
  bool Pol = CondBr->getOperand(1).getImm() != 0;
  if (!F) {
    if (!MBB.isLayoutSuccessor(T))
      cond(T, Pol);
    return;
  }
  if (T == F) {
    if (!MBB.isLayoutSuccessor(T))
      uncond(T);
    return;
  }
  if (MBB.isLayoutSuccessor(T)) {
    cond(F, !Pol);
    return;
  }
  if (MBB.isLayoutSuccessor(F)) {
    cond(T, Pol);
    return;
  }
  // Both successors need a jump.  A jump is taken on a non-zero condition
  // when its target is backward and on zero when forward; make the
  // conditional jump the one whose direction matches its polarity so that no
  // negation is needed.
  bool NegT = Pol != isBackward(&MBB, T);
  bool NegF = !Pol != isBackward(&MBB, F);
  if (!NegT || NegF) {
    uncond(F);
    cond(T, Pol);
  } else {
    uncond(T);
    cond(F, !Pol);
  }
}

static bool isNegatingCompare(unsigned Opc) {
  switch (Opc) {
  case Scry::CMP_NE_GR8: case Scry::CMP_NE_GR16: case Scry::CMP_NE_GR32: case Scry::CMP_NE_GR64:
  case Scry::CMP_ULE_GR8: case Scry::CMP_ULE_GR16: case Scry::CMP_ULE_GR32: case Scry::CMP_ULE_GR64:
  case Scry::CMP_UGE_GR8: case Scry::CMP_UGE_GR16: case Scry::CMP_UGE_GR32: case Scry::CMP_UGE_GR64:
  case Scry::CMP_SLE_GR8: case Scry::CMP_SLE_GR16: case Scry::CMP_SLE_GR32: case Scry::CMP_SLE_GR64:
  case Scry::CMP_SGE_GR8: case Scry::CMP_SGE_GR16: case Scry::CMP_SGE_GR32: case Scry::CMP_SGE_GR64:
    return true;
  default:
    return false;
  }
}

/// A jump that must negate its condition and is the only user of a compare
/// ending in a negating eq simply takes the un-negated compare result.
void ScryTemporalLowering::planNegationFolds(MachineBasicBlock &MBB,
                                             ArrayRef<PlannedJump> Jumps) {
  for (const PlannedJump &J : Jumps) {
    if (!J.Cond.isValid() || !jumpNeedsNegation(MBB, J))
      continue;
    MachineInstr *Def = MRI->getVRegDef(J.Cond);
    if (Def && Def->getParent() == &MBB && isNegatingCompare(Def->getOpcode()) &&
        MRI->hasOneNonDBGUse(J.Cond))
      FoldedNegations.insert(Def);
  }
}

/// Build the instruction sequence of jump J in the given form:
///   near, unconditional:  jmp t, trig
///   near, conditional:    ld.s c ; [eq =>0] ; jmp t, trig
///   far, unconditional:   const i8/i16/i32 t (grow ...) ; const u8 pol ; jmp
///   far, conditional:     ld.s c ; eq/echo =>n ; const ... ; jmp
/// The condition is delivered to the jmp as its first operand; the far forms
/// deliver the target as the second operand (const forwards the condition
/// behind its own value, so the polarity constant goes last).
void ScryTemporalLowering::buildJumpSeq(MachineBasicBlock &MBB,
                                        const PlannedJump &J, JumpForm Form,
                                        std::vector<Item> &Seq) {
  BlockSeq Tmp{&MBB, {}, 0};
  BlockSeq *Saved = Cur;
  Cur = &Tmp;

  bool Backward = isBackward(&MBB, J.Target);
  unsigned ChainLen =
      Form == JF_Near ? 0 : Form == JF_Far8 ? 1 : Form == JF_Far16 ? 2 : 4;

  if (J.Cond.isValid()) {
    bool Neg = jumpNeedsNegation(MBB, J) &&
               !FoldedNegations.count(MRI->getVRegDef(J.Cond));
    if (Neg)
      ++FnNegations;
    unsigned Off = slotOffset(J.Cond);
    unsigned TC = utype(J.Cond);
    if (Off / scaleOf(J.Cond) <= 31) {
      emit(Scry::LDS, {(int64_t)TC, (int64_t)(Off / scaleOf(J.Cond))});
      if (Neg)
        emit(Scry::EQ, {(int64_t)ChainLen});
      else if (ChainLen)
        emit(Scry::ECHO, {(int64_t)ChainLen, (int64_t)ChainLen});
    } else {
      emitFrameAddr(Off);
      if (Neg) {
        emit(Scry::LD, {(int64_t)TC, 0});
        emit(Scry::EQ, {(int64_t)ChainLen});
      } else {
        emit(Scry::LD, {(int64_t)TC, (int64_t)ChainLen});
      }
    }
  }

  SmallVector<int, 5> Pieces;
  if (ChainLen) {
    unsigned TC = ChainLen == 1 ? Scry::T_I8
                  : ChainLen == 2 ? Scry::T_I16
                                  : Scry::T_I32;
    for (int B = (int)ChainLen - 1; B >= 0; --B) {
      int Idx = B == (int)ChainLen - 1 ? emit(Scry::CONST, {(int64_t)TC, 0})
                                       : emit(Scry::GROW, {0});
      Cur->Items[Idx].Deferred = Item::JumpByte;
      Cur->Items[Idx].JumpByteIdx = B;
      Pieces.push_back(Idx);
    }
    if (!J.Cond.isValid()) {
      int P = emit(Scry::CONST, {(int64_t)Scry::T_U8, 0});
      Cur->Items[P].Deferred = Item::JumpPolarity;
      Pieces.push_back(P);
    }
    ++FnFarJumps;
  } else {
    ++FnNearJumps;
  }
  int Jmp = emit(Scry::JMP, {0, 0});
  Cur->Items[Jmp].JumpTarget = J.Target;
  Cur->Items[Jmp].JumpKey = J.Key;
  Cur->Items[Jmp].FarJump = ChainLen > 0;
  for (int P : Pieces)
    Cur->Items[P].JumpItem = Jmp;

  Seq = std::move(Tmp.Items);
  Cur = Saved;
}

/// Insert Seq (whose references are relative to its own start) before item
/// Pos of the current block, shifting the references of everything after.
void ScryTemporalLowering::insertItems(unsigned Pos, std::vector<Item> &Seq,
                                       const PlannedJump &J) {
  int N = (int)Seq.size();
  auto Shift = [&](int &T) {
    if (T >= (int)Pos)
      T += N;
  };
  for (Item &I : Cur->Items) {
    if (I.RefOp >= 0)
      Shift(I.RefTarget);
    if (I.Ref2Op >= 0)
      Shift(I.Ref2Target);
    if (I.JumpItem >= 0)
      Shift(I.JumpItem);
    assert(I.TrigTarget < 0 && "triggers are bound after insertion");
  }
  for (Item &I : Seq) {
    if (I.RefOp >= 0)
      I.RefTarget += Pos;
    if (I.Ref2Op >= 0)
      I.Ref2Target += Pos;
    if (I.JumpItem >= 0)
      I.JumpItem += Pos;
  }
  if (!Seq.empty()) {
    raw_string_ostream OS(Seq.front().Comment);
    OS << "jump" << (J.Cond.isValid() ? (J.TakenOnNonZero ? " if non-zero" : " if zero") : "")
       << " -> " << printMBBReference(*J.Target);
  }
  Cur->Items.insert(Cur->Items.begin() + Pos,
                    std::make_move_iterator(Seq.begin()),
                    std::make_move_iterator(Seq.end()));
}

/// Issue the jumps of MBB into the already expanded block body.  Boundaries
/// lists the item indices at which a jump may be inserted (after the prologue
/// and after each non-terminator instruction).  Every jump triggers after the
/// block's last instruction and is issued at the earliest boundary that its
/// trigger field can express and at which its condition is available.
void ScryTemporalLowering::insertJumps(MachineBasicBlock &MBB,
                                       ArrayRef<unsigned> Boundaries,
                                       ArrayRef<PlannedJump> Jumps) {
  if (Jumps.empty())
    return;

  unsigned BodySize = Cur->Items.size();
  assert(Boundaries.back() == BodySize);
  auto BoundaryAtLeast = [&](unsigned Min) {
    for (unsigned B : Boundaries)
      if (B >= Min)
        return B;
    return BodySize;
  };
  auto TrigLimit = [](JumpForm F) { return F == JF_Near ? 63u : 8191u; };

  std::vector<Item> Seqs[2];
  JumpForm Forms[2];
  unsigned Pos[2] = {0, 0};
  for (unsigned i = 0, e = Jumps.size(); i != e; ++i) {
    Forms[i] = JumpForms.lookup(Jumps[i].Key);
    buildJumpSeq(MBB, Jumps[i], Forms[i], Seqs[i]);
  }
  unsigned Earliest[2] = {Boundaries[Jumps[0].EarliestBoundary], 0};
  if (Jumps.size() == 1) {
    unsigned L = TrigLimit(Forms[0]);
    Pos[0] = BoundaryAtLeast(std::max(Earliest[0], BodySize > L ? BodySize - L : 0));
  } else {
    Earliest[1] = Boundaries[Jumps[1].EarliestBoundary];
    // The first (unconditional) jump also sees the second sequence between
    // its jmp and the trigger.
    unsigned L0 = TrigLimit(Forms[0]), N1 = Seqs[1].size();
    Pos[0] = BoundaryAtLeast(
        std::max(Earliest[0], BodySize + N1 > L0 ? BodySize + N1 - L0 : 0));
    unsigned L1 = TrigLimit(Forms[1]);
    Pos[1] = BoundaryAtLeast(
        std::max({Earliest[1], Pos[0], BodySize > L1 ? BodySize - L1 : 0}));
    insertItems(Pos[1], Seqs[1], Jumps[1]);
  }
  insertItems(Pos[0], Seqs[0], Jumps[0]);

  // Everything triggers after the block's last instruction.
  int End = (int)Cur->Items.size() - 1;
  for (Item &I : Cur->Items)
    if (I.JumpTarget)
      I.TrigTarget = End;
}

void ScryTemporalLowering::expandInstr(MachineInstr &MI,
                                       MachineBasicBlock &MBB) {
  unsigned Before = curIndex();
  unsigned Opc = MI.getOpcode();

  switch (Opc) {
  // Already handled in the prologue / no code.
  case Scry::ARG_8:
  case Scry::ARG_16:
  case Scry::ARG_32:
  case Scry::ARG_64:
  case TargetOpcode::IMPLICIT_DEF:
  case TargetOpcode::KILL:
  case TargetOpcode::DBG_VALUE:
  case TargetOpcode::DBG_VALUE_LIST:
  case TargetOpcode::DBG_LABEL:
  case TargetOpcode::CFI_INSTRUCTION:
  case TargetOpcode::EH_LABEL:
  case TargetOpcode::GC_LABEL:
    return;

  case TargetOpcode::COPY:
  case Scry::COPY_GR8:
  case Scry::COPY_GR16:
  case Scry::COPY_GR32:
  case Scry::COPY_GR64:
    emitLoadSlot(MI.getOperand(1).getReg());
    emitStoreSlot(MI.getOperand(0).getReg());
    break;

  case Scry::CONST_8:
  case Scry::CONST_16:
  case Scry::CONST_32:
  case Scry::CONST_64: {
    Register D = MI.getOperand(0).getReg();
    unsigned Scale = scaleOf(D);
    uint64_t Mask = Scale == 8 ? ~0ull : ((1ull << (8 * Scale)) - 1);
    uint64_t V = (uint64_t)MI.getOperand(1).getImm() & Mask;
    emitConstBytes(utype(D), V, Scale);
    emitStoreSlot(D);
    break;
  }

  case Scry::ADDR_SYM: {
    Register D = MI.getOperand(0).getReg();
    const MachineOperand &MO = MI.getOperand(1);
    for (int B = 3; B >= 0; --B) {
      int Idx = B == 3 ? emit(Scry::CONST, {(int64_t)Scry::T_U32, 0})
                       : emit(Scry::GROW, {0});
      Item &I = Cur->Items[Idx];
      I.SymByte = B;
      I.SymOffset = MO.getOffset();
      if (MO.isGlobal())
        I.GV = MO.getGlobal();
      else if (MO.isSymbol())
        I.ExtSym = MO.getSymbolName();
      else if (MO.isBlockAddress())
        I.BA = MO.getBlockAddress();
      else if (MO.isCPI())
        I.CPIdx = MO.getIndex();
      else
        report_fatal_error("Scry: unexpected ADDR_SYM operand");
    }
    emitStoreSlot(D);
    break;
  }

  case Scry::ADDR_FI: {
    const MachineOperand &MO = MI.getOperand(1);
    if (!MO.isImm())
      report_fatal_error("Scry: unresolved frame index");
    emitFrameAddr((unsigned)MO.getImm());
    emitStoreSlot(MI.getOperand(0).getReg());
    break;
  }

  case Scry::ADDR_OUT:
    emitFrameAddr(PrivSize + (unsigned)MI.getOperand(1).getImm());
    emitStoreSlot(MI.getOperand(0).getReg());
    break;

  case Scry::ADD_GR8: case Scry::ADD_GR16: case Scry::ADD_GR32: case Scry::ADD_GR64:
    expandBinary(MI, Scry::ADD_L);
    break;
  case Scry::SUB_GR8: case Scry::SUB_GR16: case Scry::SUB_GR32: case Scry::SUB_GR64:
    expandBinary(MI, Scry::SUB_L);
    break;
  case Scry::MUL_GR8: case Scry::MUL_GR16: case Scry::MUL_GR32: case Scry::MUL_GR64:
    expandBinary(MI, Scry::MUL_L);
    break;
  case Scry::AND_GR8: case Scry::AND_GR16: case Scry::AND_GR32: case Scry::AND_GR64:
    expandBinary(MI, Scry::AND);
    break;
  case Scry::OR_GR8: case Scry::OR_GR16: case Scry::OR_GR32: case Scry::OR_GR64:
    expandBinary(MI, Scry::OR);
    break;
  case Scry::XOR_GR8: case Scry::XOR_GR16: case Scry::XOR_GR32: case Scry::XOR_GR64:
    expandBinary(MI, Scry::XOR);
    break;
  case Scry::UDIV_GR8: case Scry::UDIV_GR16: case Scry::UDIV_GR32: case Scry::UDIV_GR64:
    expandBinary(MI, Scry::DIV_L);
    break;
  case Scry::UREM_GR8: case Scry::UREM_GR16: case Scry::UREM_GR32: case Scry::UREM_GR64:
    expandBinary(MI, Scry::DIV_H);
    break;
  case Scry::SHL_GR8: case Scry::SHL_GR16: case Scry::SHL_GR32: case Scry::SHL_GR64:
    expandBinary(MI, Scry::SH_L);
    break;
  case Scry::LSHR_GR8: case Scry::LSHR_GR16: case Scry::LSHR_GR32: case Scry::LSHR_GR64:
    expandRightShift(MI, /*Arithmetic=*/false);
    break;
  case Scry::ASHR_GR8: case Scry::ASHR_GR16: case Scry::ASHR_GR32: case Scry::ASHR_GR64:
    expandRightShift(MI, /*Arithmetic=*/true);
    break;

  case Scry::CMP_EQ_GR8: case Scry::CMP_EQ_GR16: case Scry::CMP_EQ_GR32: case Scry::CMP_EQ_GR64:
    expandCompare(MI, Scry::EQ, false, false);
    break;
  case Scry::CMP_NE_GR8: case Scry::CMP_NE_GR16: case Scry::CMP_NE_GR32: case Scry::CMP_NE_GR64:
    expandCompare(MI, Scry::EQ, true, false);
    break;
  case Scry::CMP_ULT_GR8: case Scry::CMP_ULT_GR16: case Scry::CMP_ULT_GR32: case Scry::CMP_ULT_GR64:
    expandCompare(MI, Scry::LT, false, false);
    break;
  case Scry::CMP_UGT_GR8: case Scry::CMP_UGT_GR16: case Scry::CMP_UGT_GR32: case Scry::CMP_UGT_GR64:
    expandCompare(MI, Scry::GT, false, false);
    break;
  case Scry::CMP_ULE_GR8: case Scry::CMP_ULE_GR16: case Scry::CMP_ULE_GR32: case Scry::CMP_ULE_GR64:
    expandCompare(MI, Scry::GT, true, false);
    break;
  case Scry::CMP_UGE_GR8: case Scry::CMP_UGE_GR16: case Scry::CMP_UGE_GR32: case Scry::CMP_UGE_GR64:
    expandCompare(MI, Scry::LT, true, false);
    break;
  case Scry::CMP_SLT_GR8: case Scry::CMP_SLT_GR16: case Scry::CMP_SLT_GR32: case Scry::CMP_SLT_GR64:
    expandCompare(MI, Scry::LT, false, true);
    break;
  case Scry::CMP_SGT_GR8: case Scry::CMP_SGT_GR16: case Scry::CMP_SGT_GR32: case Scry::CMP_SGT_GR64:
    expandCompare(MI, Scry::GT, false, true);
    break;
  case Scry::CMP_SLE_GR8: case Scry::CMP_SLE_GR16: case Scry::CMP_SLE_GR32: case Scry::CMP_SLE_GR64:
    expandCompare(MI, Scry::GT, true, true);
    break;
  case Scry::CMP_SGE_GR8: case Scry::CMP_SGE_GR16: case Scry::CMP_SGE_GR32: case Scry::CMP_SGE_GR64:
    expandCompare(MI, Scry::LT, true, true);
    break;

  case Scry::SELECT_GR8: case Scry::SELECT_GR16: case Scry::SELECT_GR32: case Scry::SELECT_GR64: {
    // pick: condition zero -> first value, so route [c, false, true].
    Register D = MI.getOperand(0).getReg();
    emitLoadSlot(MI.getOperand(1).getReg());
    int E1 = emit(Scry::ECHO, {0, 0});
    emitLoadSlot(MI.getOperand(3).getReg());
    int E2 = emit(Scry::ECHO, {0, 0});
    emitLoadSlot(MI.getOperand(2).getReg());
    int P = emit(Scry::PICK, {0});
    setRef(E1, P); setRef(E1, P, true);
    setRef(E2, P); setRef(E2, P, true);
    emitStoreSlot(D);
    break;
  }

  case Scry::ZEXT_8_16: case Scry::ZEXT_8_32: case Scry::ZEXT_8_64:
  case Scry::ZEXT_16_32: case Scry::ZEXT_16_64: case Scry::ZEXT_32_64:
  case Scry::TRUNC_16_8: case Scry::TRUNC_32_8: case Scry::TRUNC_64_8:
  case Scry::TRUNC_32_16: case Scry::TRUNC_64_16: case Scry::TRUNC_64_32:
    expandCasts(MI, {utype(MI.getOperand(0).getReg())});
    break;
  case Scry::SEXT_8_16: case Scry::SEXT_8_32: case Scry::SEXT_8_64:
  case Scry::SEXT_16_32: case Scry::SEXT_16_64: case Scry::SEXT_32_64:
    expandCasts(MI, {stype(MI.getOperand(1).getReg()),
                     utype(MI.getOperand(0).getReg())});
    break;
  case Scry::SEXTINREG_16: case Scry::SEXTINREG_32: case Scry::SEXTINREG_64: {
    unsigned Bits = MI.getOperand(2).getImm();
    unsigned L = Log2_32(Bits / 8);
    expandCasts(MI, {Scry::unsignedTypeCode(L), Scry::signedTypeCode(L),
                     utype(MI.getOperand(0).getReg())});
    break;
  }

  case Scry::LD_8: case Scry::LD_16: case Scry::LD_32: case Scry::LD_64: {
    Register D = MI.getOperand(0).getReg();
    emitLoadSlot(MI.getOperand(1).getReg());
    emit(Scry::LD, {(int64_t)utype(D), 0});
    emitStoreSlot(D);
    break;
  }
  case Scry::ST_8: case Scry::ST_16: case Scry::ST_32: case Scry::ST_64: {
    emitLoadSlot(MI.getOperand(0).getReg());
    int E = emit(Scry::ECHO, {0, 0});
    emitLoadSlot(MI.getOperand(1).getReg());
    int S = emit(Scry::ST);
    setRef(E, S); setRef(E, S, true);
    break;
  }

  case Scry::BR:
  case Scry::BRCOND:
    return; // issued by insertJumps once the block body is known

  case Scry::RETURN:
    expandReturn(MI);
    break;

  case Scry::CALL_V: case Scry::CALL_8: case Scry::CALL_16: case Scry::CALL_32: case Scry::CALL_64:
  case Scry::CALLI_V: case Scry::CALLI_8: case Scry::CALLI_16: case Scry::CALLI_32: case Scry::CALLI_64:
    expandCall(MI);
    break;

  case Scry::OUT_RSRV:
    emitReserves((unsigned)MI.getOperand(0).getImm(), /*Private=*/false);
    break;
  case Scry::OUT_FREE: {
    unsigned Bytes = (unsigned)MI.getOperand(0).getImm();
    for (unsigned K = 0; K < 16; ++K)
      if (Bytes & (1u << K))
        emit(Scry::FREE, {(int64_t)K});
    break;
  }

  default:
    if (MI.isPseudo())
      report_fatal_error(Twine("Scry: cannot lower pseudo instruction ") +
                         TII->getName(Opc));
    // A real instruction (e.g. trap): copy it verbatim.
    {
      Item I;
      I.Opc = Opc;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isImm())
          I.Imms.push_back(MO.getImm());
      Cur->Items.push_back(std::move(I));
    }
    break;
  }

  if (curIndex() > (int)Before)
    Cur->Items[Before].Comment = describe(MI);
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

bool ScryTemporalLowering::expandFunction() {
  Blocks.clear();
  FoldedNegations.clear();
  FnNearJumps = FnFarJumps = FnNegations = 0;
  Blocks.reserve(MF->size());
  for (MachineBasicBlock &MBB : *MF) {
    Blocks.push_back(BlockSeq{&MBB, {}, 0});
    Cur = &Blocks.back();
    if (&MBB == &MF->front())
      expandPrologue(MBB);
    SmallVector<PlannedJump, 2> Jumps;
    planJumps(MBB, Jumps);
    planNegationFolds(MBB, Jumps);
    SmallVector<unsigned, 64> Boundaries;
    Boundaries.push_back(curIndex());
    for (MachineInstr &MI : MBB) {
      expandInstr(MI, MBB);
      if (!MI.isTerminator())
        Boundaries.push_back(curIndex());
    }
    insertJumps(MBB, Boundaries, Jumps);
    // Every block occupies at least one instruction so that a forward jump
    // never targets the instruction right after its own trigger.
    if (Cur->Items.empty())
      emit(Scry::NOP);
  }

  // Resolve local references and triggers, and lay out the blocks.
  unsigned Abs = 0;
  for (BlockSeq &B : Blocks) {
    B.Start = Abs;
    for (unsigned i = 0, e = B.Items.size(); i != e; ++i) {
      Item &I = B.Items[i];
      if (I.RefOp >= 0) {
        int Ref = I.RefTarget - (int)i - 1;
        assert(Ref >= 0 && "backward reference");
        if (I.Opc == Scry::ECHO && Ref > 31) {
          // Out of range for the short echo: use the long form, which
          // forwards all inputs (equivalent for single values).
          I.Opc = Scry::ECHOL;
          I.Imms.resize(1);
          I.Ref2Op = -1;
        }
        if (Ref > (I.Opc == Scry::ECHOL ? 1023 : 31))
          report_fatal_error("Scry: forward reference out of range");
        I.Imms[I.RefOp] = Ref;
      }
      if (I.Ref2Op >= 0) {
        int Ref = I.Ref2Target - (int)i - 1;
        assert(Ref >= 0 && Ref <= 31 && "bad second reference");
        I.Imms[I.Ref2Op] = Ref;
      }
      if (I.TrigTarget >= 0) {
        int Loc = I.TrigTarget - (int)i;
        assert(Loc >= 0 && "trigger before the jump");
        if (!I.FarJump) {
          if (Loc > 63)
            report_fatal_error("Scry: near jump trigger out of range");
          I.Imms[1] = Loc;
        } else {
          if (Loc > 8191)
            report_fatal_error("Scry: jump trigger out of range");
          // The two immediates hold the 13-bit trigger offset.
          I.Imms[0] = SignExtend64<7>((uint64_t)Loc >> 6);
          I.Imms[1] = Loc & 63;
        }
      }
    }
    Abs += B.Items.size();
  }

  DenseMap<MachineBasicBlock *, unsigned> Starts;
  for (BlockSeq &B : Blocks)
    Starts[B.MBB] = B.Start;

  // Relative targets: a backward jump targets jmp + t (t <= 0) and is taken
  // on a non-zero condition; a forward jump targets trigger + t + 1 (t >= 1)
  // and is taken on zero.  A jump whose form cannot express its target moves
  // to the next form and the function is expanded again.
  bool Changed = false;
  for (BlockSeq &B : Blocks) {
    for (unsigned i = 0, e = B.Items.size(); i != e; ++i) {
      Item &I = B.Items[i];
      if (!I.JumpTarget)
        continue;
      int64_t JmpAbs = (int64_t)B.Start + i;
      int64_t TrigAbs = JmpAbs + (I.TrigTarget - (int)i);
      int64_t TargetStart = Starts[I.JumpTarget];
      int64_t TV = isBackward(B.MBB, I.JumpTarget) ? TargetStart - JmpAbs
                                                   : TargetStart - TrigAbs - 1;
      assert((isBackward(B.MBB, I.JumpTarget) ? TV <= 0 : TV >= 1) &&
             "jump direction mismatch");
      JumpForm Form = JumpForms.lookup(I.JumpKey);
      bool Fits = Form == JF_Near    ? (TV >= -64 && TV <= 63)
                  : Form == JF_Far8  ? (TV >= -128 && TV <= 127)
                  : Form == JF_Far16 ? (TV >= -32768 && TV <= 32767)
                                     : true;
      if (!Fits) {
        JumpForms[I.JumpKey] = (JumpForm)(Form + 1);
        Changed = true;
        continue;
      }
      I.TargetValue = TV;
      if (!I.FarJump)
        I.Imms[0] = TV;
    }
  }
  if (Changed)
    return false;

  // Fill in the operand-producing pieces of the far jumps.
  for (BlockSeq &B : Blocks) {
    for (Item &I : B.Items) {
      if (I.Deferred == Item::None)
        continue;
      const Item &J = B.Items[I.JumpItem];
      if (I.Deferred == Item::JumpPolarity)
        I.Imms[1] = isBackward(B.MBB, J.JumpTarget) ? 1 : 0;
      else
        I.Imms[I.Opc == Scry::CONST ? 1 : 0] =
            (J.TargetValue >> (8 * I.JumpByteIdx)) & 0xff;
    }
  }
  for (BlockSeq &B : Blocks)
    for (unsigned i = 0, e = B.Items.size(); i != e; ++i)
      if (B.Items[i].JumpTarget)
        JumpTrigDistance += B.Items[i].TrigTarget - (int)i;
  NumNearJumps += FnNearJumps;
  NumFarJumps += FnFarJumps;
  NumJumpNegations += FnNegations;
  return true;
}

void ScryTemporalLowering::resolveAndMaterialise() {
  for (BlockSeq &B : Blocks) {
    MachineBasicBlock &MBB = *B.MBB;
    // Remove the virtual instructions.
    for (auto It = MBB.begin(); It != MBB.end();)
      (It++)->eraseFromParent();
    DebugLoc DL;
    for (Item &I : B.Items) {
      MachineInstrBuilder MIB = BuildMI(&MBB, DL, TII->get(I.Opc));
      for (unsigned k = 0, e = I.Imms.size(); k != e; ++k) {
        bool IsSymImm = (I.GV || I.ExtSym || I.BA || I.CPIdx >= 0) &&
                        k == (I.Opc == Scry::CONST ? 1u : 0u);
        if (!IsSymImm) {
          MIB.addImm(I.Imms[k]);
          continue;
        }
        unsigned Flags = Scry::MO_B0 + I.SymByte;
        if (I.GV)
          MIB.addGlobalAddress(I.GV, I.SymOffset, Flags);
        else if (I.ExtSym)
          MIB.addExternalSymbol(I.ExtSym, Flags);
        else if (I.BA)
          MIB.addBlockAddress(I.BA, I.SymOffset, Flags);
        else
          MIB.addConstantPoolIndex(I.CPIdx, I.SymOffset, Flags);
      }
      if (!I.Comment.empty())
        FuncInfo->setComment(MIB.getInstr(), I.Comment);
    }
  }
}

bool ScryTemporalLowering::runOnMachineFunction(MachineFunction &Fn) {
  MF = &Fn;
  TII = MF->getSubtarget<ScrySubtarget>().getInstrInfo();
  MRI = &MF->getRegInfo();
  FuncInfo = MF->getInfo<ScryFunctionInfo>();

  const MachineFrameInfo &MFI = MF->getFrameInfo();
  if (MFI.hasVarSizedObjects())
    report_fatal_error("Scry: dynamic stack allocation is not supported");

  IncomingSize = FuncInfo->getIncomingArgsSize();
  PrivSize = alignTo(MFI.getStackSize(), 16);
  if (PrivSize < IncomingSize)
    PrivSize = IncomingSize;
  ReserveSize = PrivSize - IncomingSize;

  LayoutIndex.clear();
  unsigned Idx = 0;
  for (MachineBasicBlock &MBB : *MF)
    LayoutIndex[&MBB] = Idx++;

  // Relax the jump forms until every target fits; each round moves at least
  // one jump to a more general form.
  JumpForms.clear();
  unsigned Rounds = 0;
  while (!expandFunction()) {
    if (++Rounds > 4 * MF->size() + 16)
      report_fatal_error("Scry: jump relaxation did not converge");
  }
  resolveAndMaterialise();

  MF->getProperties().setNoVRegs();
  return true;
}
