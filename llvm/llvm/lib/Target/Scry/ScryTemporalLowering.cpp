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
// in-flight values at all.  Branches are emitted as position-independent
// "far" jumps whose relative targets are computed here from the final layout
// (all Scry instructions are exactly two bytes, so the layout is exact).
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
#include "llvm/ADT/SmallVector.h"
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

  // Deferred far-jump pieces, resolved once the block layout is known.
  enum DeferredKind { None, JumpPolarity, JumpByte };
  DeferredKind Deferred = None;
  MachineBasicBlock *JumpTarget = nullptr;
  unsigned JumpByteIdx = 0;
  int JumpDelta = 0; // distance from this item to the jmp instruction

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
  unsigned JumpWidth = 2;    // bytes of relative jump immediates

  std::vector<BlockSeq> Blocks;
  BlockSeq *Cur = nullptr;

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
  void emitFarJump(MachineBasicBlock *Target);
  unsigned farJumpLen() const { return 3 + JumpWidth; }
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
  void expandBranch(MachineInstr &MI, MachineBasicBlock &MBB);

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

/// Emit an unconditional jump to Target whose relative displacement is filled
/// in after layout.  Sequence (JumpWidth = 2):
///   const u8, c ; echo =>2, =>2 ; const i16, b1 ; grow b0 ; jmp 0, 0
void ScryTemporalLowering::emitFarJump(MachineBasicBlock *Target) {
  int Pol = emit(Scry::CONST, {(int64_t)Scry::T_U8, 0});
  Cur->Items[Pol].Deferred = Item::JumpPolarity;
  Cur->Items[Pol].JumpTarget = Target;
  Cur->Items[Pol].JumpDelta = (int)JumpWidth + 2;

  emit(Scry::ECHO, {(int64_t)JumpWidth, (int64_t)JumpWidth});

  unsigned TypeCode = JumpWidth == 2 ? Scry::T_I16 : Scry::T_I32;
  for (int B = (int)JumpWidth - 1; B >= 0; --B) {
    int Idx = B == (int)JumpWidth - 1 ? emit(Scry::CONST, {(int64_t)TypeCode, 0})
                                      : emit(Scry::GROW, {0});
    Cur->Items[Idx].Deferred = Item::JumpByte;
    Cur->Items[Idx].JumpTarget = Target;
    Cur->Items[Idx].JumpByteIdx = B;
    Cur->Items[Idx].JumpDelta = B + 1;
  }
  emit(Scry::JMP, {0, 0});
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
  if (Negate)
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

void ScryTemporalLowering::expandBranch(MachineInstr &MI,
                                        MachineBasicBlock &MBB) {
  if (MI.getOpcode() == Scry::BR) {
    MachineBasicBlock *T = MI.getOperand(0).getMBB();
    if (!MBB.isLayoutSuccessor(T))
      emitFarJump(T);
    return;
  }

  // BRCOND $cond, T: branch if cond != 0.
  Register C = MI.getOperand(0).getReg();
  MachineBasicBlock *T = MI.getOperand(1).getMBB();
  if (MBB.isLayoutSuccessor(T)) {
    // "if (c) goto next; else <BR F follows>": skip the following
    // unconditional jump when c != 0.  A forward jmp is taken when its
    // operand is zero, so negate c with eq.
    MachineBasicBlock::iterator Next = std::next(MI.getIterator());
    if (Next == MBB.end() || Next->getOpcode() != Scry::BR ||
        Next->getOperand(0).getMBB() == T)
      return; // redundant: falls through anyway
    emitLoadSlot(C);
    emit(Scry::EQ, {0});
    emit(Scry::JMP, {(int64_t)farJumpLen(), 0});
    return;
  }
  emitLoadSlot(C);
  emit(Scry::ECHO, {0, 0});
  emit(Scry::JMP, {(int64_t)farJumpLen(), 0}); // skip far jump when c == 0
  emitFarJump(T);
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
    expandBranch(MI, MBB);
    break;

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
  Blocks.reserve(MF->size());
  for (MachineBasicBlock &MBB : *MF) {
    Blocks.push_back(BlockSeq{&MBB, {}, 0});
    Cur = &Blocks.back();
    if (&MBB == &MF->front())
      expandPrologue(MBB);
    for (MachineInstr &MI : MBB)
      expandInstr(MI, MBB);
  }

  // Resolve local references and lay out the blocks.
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
    }
    Abs += B.Items.size();
  }

  // Far jumps need 32-bit displacements in very large functions.
  if (JumpWidth == 2 && Abs >= 32000)
    return false;

  DenseMap<MachineBasicBlock *, unsigned> Starts;
  for (BlockSeq &B : Blocks)
    Starts[B.MBB] = B.Start;

  for (BlockSeq &B : Blocks) {
    for (unsigned i = 0, e = B.Items.size(); i != e; ++i) {
      Item &I = B.Items[i];
      if (I.Deferred == Item::None)
        continue;
      int64_t JmpAbs = (int64_t)B.Start + i + I.JumpDelta;
      int64_t Dist = (int64_t)Starts[I.JumpTarget] - JmpAbs;
      // Backward (or self) jumps are taken when the condition is non-zero
      // and target jmp + 2*t; forward jumps are taken when the condition is
      // zero and target jmp + 2*(t + 1).
      int64_t TargetVal;
      int64_t Polarity;
      if (Dist <= 0) {
        TargetVal = Dist;
        Polarity = 1;
      } else {
        TargetVal = Dist - 1;
        Polarity = 0;
      }
      if (I.Deferred == Item::JumpPolarity)
        I.Imms[1] = Polarity;
      else
        I.Imms[I.Opc == Scry::CONST ? 1 : 0] =
            (TargetVal >> (8 * I.JumpByteIdx)) & 0xff;
    }
  }
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

  JumpWidth = 2;
  if (!expandFunction()) {
    JumpWidth = 4;
    if (!expandFunction())
      report_fatal_error("Scry: function too large");
  }
  resolveAndMaterialise();

  MF->getProperties().setNoVRegs();
  return true;
}
