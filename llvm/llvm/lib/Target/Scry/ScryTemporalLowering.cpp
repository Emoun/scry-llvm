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
// are whatever earlier instructions aimed at it, ordered by the execution
// time of their last emitters.
//
// Values with a single definition whose uses all follow it in the same block
// stay *in flight*: the producer's reference is bound to the consumer when
// the consumer is emitted.  A value with several consumers is copied with
// `dup`, a value whose consumer is more than 31 instructions away is carried
// by an `echo` hop, and operands that would arrive in the wrong order are
// re-emitted by a hop so that their last emitters execute in operand order.
// Constants and addresses are next-only producers; they are glued right in
// front of their consumer and, since `const` forwards its ready inputs behind
// its own value, earlier values can be aimed at the chain to follow the
// constant in the consumer's operand list.
//
//     %d = ADD %a, %b            add Low =>0     ; a and b aimed here
//     %e = MUL %d, 7             const u32, 7    ; d aimed at the mul
//                                mul Low =>5     ; -> the consumer 5 later
//
// Values live across blocks (PHI copies, loop-carried values, values used in
// several blocks) get a frame slot: the definition stores it right away and
// every use loads it again (`ld.s`, or `saddr`+`ld` outside the short range).
//
// Branches: a Scry jump is *issued* ahead of time and only takes effect after
// the instruction at its trigger address.  The jumps of a block trigger after
// the block's last instruction and are issued as early as possible: an
// unconditional jump at the block start, a conditional one right after its
// condition is produced.  A near jump (target within 64 instructions, trigger
// within 63) is a single instruction; otherwise the target is an operand
// produced by a glued const/grow chain and the trigger offset is 13 bits.
// The form of each jump is chosen by relaxation once the layout is known (all
// Scry instructions are exactly two bytes, so the layout is exact).  When
// both successors need a jump, an unconditional jump is issued first and a
// conditional one with the same trigger overrides it when taken.
//
// Function entry: the arguments delivered to the first instruction are
// spread with echo to their consumers (or slot stores) after the private
// frame has been reserved.  Calls aim the arguments at the instruction after
// the call (the trigger slot), deliver the callee address to the call itself
// and capture the return value with an echo at the trigger slot.  Returns aim
// the values at the instruction after `ret 0`, preceded by freeing the frame.
// A call shifts the caller's pending operand lists by one (the list meant for
// the trigger slot becomes the callee's arguments and, on return, the next
// list is merged in front of the return values), so a reference that crosses
// a call is one larger than the instruction distance.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/ScryMCTargetDesc.h"
#include "Scry.h"
#include "ScryInstrInfo.h"
#include "ScryMachineFunctionInfo.h"
#include "ScrySubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
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
#include <array>
#include <map>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "scry-temporal"

static cl::opt<bool> ElideCasts(
    "scry-elide-casts", cl::Hidden, cl::init(true),
    cl::desc("Drop zero extensions whose uses do not depend on the width"));

static cl::opt<unsigned> ElideCastsLimit(
    "scry-elide-casts-limit", cl::Hidden, cl::init(~0u),
    cl::desc("Elide at most this many zero extensions (for bisection)"));

static cl::opt<bool> ExpireAllPending(
    "scry-expire-all-pending", cl::Hidden, cl::init(false),
    cl::desc("Expire outputs with every pending output as the hop margin "
             "instead of iterating to the outputs actually due (the previous "
             "behaviour, for measurement)"));


STATISTIC(NumNearJumps, "Number of single-instruction (near) jumps");
STATISTIC(NumFarJumps, "Number of two-operand (far) jumps");
STATISTIC(NumJumpNegations, "Number of jump conditions negated with eq");
STATISTIC(JumpTrigDistance, "Sum of jump issue-to-trigger distances");
STATISTIC(NumHops, "Number of echo hops (reach, ordering, capture)");
STATISTIC(NumDups, "Number of dup copies");
STATISTIC(NumSlotLoads, "Number of slot loads");
STATISTIC(NumSlotStores, "Number of slot stores");
STATISTIC(NumElidedCasts, "Number of zero extensions not emitted");

namespace {

/// A real instruction waiting to be materialised.
struct Item {
  unsigned Opc = 0;
  SmallVector<int64_t, 2> Imms;

  // Pending forward references: Imms[RefOp] := Target - index - 1 plus one
  // per call crossed (not counting the call whose arguments they are).
  int RefOp = -1;
  int RefTarget = -1;
  bool RefArg = false;
  int Ref2Op = -1;
  int Ref2Target = -1;
  bool Ref2Arg = false;

  // Symbol operand of CONST/GROW (ADDR_SYM expansion).
  const GlobalValue *GV = nullptr;
  const char *ExtSym = nullptr;
  const BlockAddress *BA = nullptr;
  int CPIdx = -1;
  int64_t SymOffset = 0;
  unsigned SymByte = 0;
  bool PCRel = false; // the byte is of (symbol - this instruction's address)

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
  std::vector<int> Calls; // indices of the call items
  unsigned Start = 0;     // absolute index of the first item within the function

  /// Number of calls strictly between items E and C.
  int callsBetween(int E, int C) const {
    int N = 0;
    for (int K : Calls)
      if (K > E && K < C)
        ++N;
    return N;
  }
  /// Reference value delivering from item E to item C.
  int refValue(int E, int C, bool IsArg) const {
    return C - E - 1 + callsBetween(E, C) - (IsArg ? 1 : 0);
  }
};

/// An output of an item whose reference has not been bound to a consumer yet.
struct Output {
  int Item;   // emitter
  int RefOp;  // immediate holding the reference
  int Reach;  // largest reference the immediate can hold
};

/// A value in flight within the current block.
struct ValueState {
  SmallVector<Output, 2> Pending; // unbound outputs carrying the value
  unsigned RemainingUses = 0;     // consumer operands still to be served
};

/// An operand of a consumer being emitted.
struct Operand {
  enum Kind {
    Flight,    // value in flight (V)
    Slot,      // value in the frame slot of Reg
    Const,     // immediate constant: TypeCode, Imm, Scale
    FrameAddr, // address of private-frame offset Imm
    SymAddr,   // address of a symbol (Sym)
  };
  Kind K = Flight;
  unsigned V = 0;
  Register Reg;
  unsigned TypeCode = 0;
  uint64_t Imm = 0;
  unsigned Scale = 1;
  const MachineOperand *Sym = nullptr;
  // A symbol address consumed directly as a memory base or call target: when
  // built right in front of that consumer it is PC-relative (three
  // instructions instead of four).
  bool RelBase = false;

  static Operand flight(unsigned V) {
    Operand O;
    O.K = Flight;
    O.V = V;
    return O;
  }
  static Operand slot(Register R) {
    Operand O;
    O.K = Slot;
    O.Reg = R;
    return O;
  }
  static Operand constant(unsigned TypeCode, uint64_t Imm, unsigned Scale) {
    Operand O;
    O.K = Const;
    O.TypeCode = TypeCode;
    O.Imm = Imm;
    O.Scale = Scale;
    return O;
  }
  static Operand frameAddr(unsigned Off) {
    Operand O;
    O.K = FrameAddr;
    O.Imm = Off;
    return O;
  }
  static Operand symAddr(const MachineOperand *MO) {
    Operand O;
    O.K = SymAddr;
    O.Sym = MO;
    return O;
  }
  bool isChain() const { return K != Flight; }
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
  MachineBasicBlock *CurMBB = nullptr;
  DenseMap<const MachineInstr *, unsigned> MIIndex; // position within block

  // --- values in flight ----------------------------------------------------
  std::vector<ValueState> Values;
  DenseMap<Register, unsigned> RegValue;   // register -> value in flight
  DenseMap<Register, Register> SlotAlias;  // copy of a slot value -> its slot
  const MachineInstr *CurExpandMI = nullptr; // virtual instruction being expanded
  DenseSet<Register> NarrowRegs; // registers holding a narrower value than their class
  unsigned ElidedBefore = 0; // casts elided in the functions already done (for the limit)
  DenseMap<Register, const MachineInstr *> Remat; // constants built at use
  unsigned FnHops = 0, FnDups = 0, FnSlotLoads = 0, FnSlotStores = 0,
           FnElidedCasts = 0;

  unsigned newValue(unsigned Uses) {
    Values.push_back(ValueState{{}, Uses});
    return Values.size() - 1;
  }
  bool hasSlot(Register R) const { return FuncInfo->getSpillSlot(R) >= 0; }
  unsigned countUsesAfter(Register R, const MachineInstr &Def) const;
  unsigned countUsesFromStart(Register R) const;
  bool definedLaterInBlock(Register R, const MachineInstr *After) const;
  /// Registers delivered in flight to the successors of the current block,
  /// and the number of uses to count for a definition of R by MI.
  DenseSet<Register> DeliverSet;
  bool deliveredAtEnd(Register R, const MachineInstr *Def) const {
    return DeliverSet.count(R) && !definedLaterInBlock(R, Def);
  }
  unsigned extraUses(Register R, const MachineInstr *Def) const {
    return (hasSlot(R) ? 1 : 0) + (deliveredAtEnd(R, Def) ? 1 : 0);
  }
  void computeDeliverSet(MachineBasicBlock &MBB);
  void emitLandingPads(MachineBasicBlock &MBB);
  void deliverContract(MachineBasicBlock &MBB);
  // Edge flight on the final control flow graph: the registers live into
  // each block that travel in flight, and each such register's landing
  // position (the slot of the pair is always 0 now).
  void planEdgeFlight();
  DenseMap<const MachineBasicBlock *, SmallVector<Register, 8>> EdgeLiveIn;
  DenseMap<Register, std::pair<unsigned, unsigned>> EdgePos;
  ArrayRef<Register> edgeLiveIns(const MachineBasicBlock *MBB) const {
    auto It = EdgeLiveIn.find(MBB);
    return It == EdgeLiveIn.end() ? ArrayRef<Register>() : ArrayRef<Register>(It->second);
  }
  // Deferred bindings of a contract delivery: (output, landing position).
  SmallVector<std::pair<Output, int>, 8> *DeferredBinds = nullptr;
  Operand operandFor(const MachineOperand &MO);
  void produce(unsigned V, int ItemIdx, int RefOp, int Reach);
  void bindOutput(const Output &O, int Consumer, bool IsArg = false);
  int takeOutput(unsigned V, int After, Output &O);
  void hopValue(unsigned V, unsigned Idx);
  void ensureOutputs(unsigned V, unsigned Count);
  void ensureDirect(unsigned V);
  void expire(unsigned Lookahead, unsigned ExtraCalls = 0);
  int prepareOperands(ArrayRef<Operand> Ops, unsigned Gap, bool IsArg = false);
  int consume(unsigned Opc, ArrayRef<Operand> Ops,
              std::initializer_list<int64_t> Imms = {});
  int consumeCommutative(unsigned Opc, Operand A, Operand B,
                         std::initializer_list<int64_t> Imms);
  int nextOutputTime(const Operand &O) const;
  unsigned chainLength(const Operand &O) const;
  bool chainPassesThrough(const Operand &O) const;
  int chainRefOp(const Operand &O) const;
  int emitChain(const Operand &O, int &LastItem, bool Glued = false);
  void relocateGlued(int First, int Consumer);
  int outputTime(const Output &O) const;

  // --- jumps ---------------------------------------------------------------
  enum JumpForm { JF_Near, JF_Far8, JF_Far16, JF_Far32 };
  DenseMap<const MachineInstr *, JumpForm> JumpForms;
  DenseMap<const MachineBasicBlock *, unsigned> LayoutIndex;
  unsigned FnNearJumps = 0, FnFarJumps = 0, FnNegations = 0; // this round

  struct PlannedJump {
    const MachineInstr *Key;
    MachineBasicBlock *Target;
    Register Cond;        // invalid: unconditional
    bool TakenOnNonZero;  // with Cond: polarity of the branch
    unsigned Boundary;    // instruction index after which it is issued
  };
  bool isBackward(const MachineBasicBlock *From,
                  const MachineBasicBlock *To) const {
    return LayoutIndex.lookup(To) <= LayoutIndex.lookup(From);
  }
  void planJumps(MachineBasicBlock &MBB, SmallVectorImpl<PlannedJump> &Jumps);
  DenseSet<const MachineInstr *> FoldedNegations;
  // Compares whose only user is a jump of their block.
  DenseSet<const MachineInstr *> JumpOnlyCompares;
  // For those: does the jump fire on a non-zero operand (backward target),
  // and does the branch fire when its condition is true (polarity)?
  DenseMap<const MachineInstr *, std::pair<bool, bool>> JumpOnlyInfo;
  bool jumpNeedsNegation(const MachineBasicBlock &MBB,
                         const PlannedJump &J) const {
    return J.TakenOnNonZero != isBackward(&MBB, J.Target);
  }
  void planNegationFolds(MachineBasicBlock &MBB, ArrayRef<PlannedJump> Jumps);
  void emitJump(MachineBasicBlock &MBB, const PlannedJump &J);

  // --- slot helpers --------------------------------------------------------
  unsigned slotOffset(Register R) const;
  unsigned scaleOf(Register R) const;
  unsigned log2Scale(Register R) const { return Log2_32(scaleOf(R)); }
  unsigned utype(Register R) const { return Scry::unsignedTypeCode(log2Scale(R)); }
  unsigned stype(Register R) const { return Scry::signedTypeCode(log2Scale(R)); }
  bool shortSlot(Register R) const {
    unsigned Off = slotOffset(R);
    return Off % scaleOf(R) == 0 && Off / scaleOf(R) <= 31;
  }

  // --- emission helpers ----------------------------------------------------
  int emit(unsigned Opc, std::initializer_list<int64_t> Imms = {});
  void setRef(int Idx, int TargetIdx, bool Second = false);
  int curIndex() const { return (int)Cur->Items.size(); }

  unsigned frameAddrLen(unsigned Off) const;
  void emitFrameAddr(unsigned Off);
  void emitConstBytes(unsigned TypeCode3, uint64_t Value, unsigned Scale);
  void emitSymAddr(const MachineOperand &MO, bool PCRel);
  void storeToSlot(Register R, unsigned V);
  void storeOperand(Register R, const Operand &Src);
  void emitFrees();
  void emitReserves(unsigned Bytes, bool Private);

  // --- expansion -----------------------------------------------------------
  void expandPrologue(MachineBasicBlock &Entry);
  void expandInstr(MachineInstr &MI, MachineBasicBlock &MBB);
  void defineResult(MachineInstr &MI, unsigned V);
  unsigned defineValue(Register D, const MachineInstr &MI);
  void expandBinary(MachineInstr &MI, unsigned RealOpc);
  void expandRightShift(MachineInstr &MI, bool Arithmetic);
  void expandCompare(MachineInstr &MI, unsigned First, bool Negate,
                     bool Signed);
  void expandCasts(MachineInstr &MI, ArrayRef<unsigned> TypeCodes);
  bool widthTolerantUse(const MachineInstr &Use, Register R) const;
  bool tryElideZext(MachineInstr &MI);
  void expandCall(MachineInstr &MI);
  void expandReturn(MachineInstr &MI);
  void expandCopy(MachineInstr &MI);
  void expandConstDef(MachineInstr &MI);

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

/// Number of instructions emitConstBytes will emit: the leading bytes that
/// the const's own extension produces (zero for unsigned types, the sign
/// for signed ones) are left out.
static unsigned constBytesLen(uint64_t Value, unsigned Scale,
                              bool Signed = false) {
  return scryConstBytesLen(Value, Scale, Signed);
}

/// Emit a constant of the given 3-bit type code; the value goes to the next
/// instruction.  Any ready inputs are forwarded behind it.
void ScryTemporalLowering::emitConstBytes(unsigned TypeCode3, uint64_t Value,
                                          unsigned Scale) {
  unsigned N = constBytesLen(Value, Scale, TypeCode3 & 1);
  emit(Scry::CONST, {(int64_t)TypeCode3, (int64_t)((Value >> (8 * (N - 1))) & 0xff)});
  for (int i = (int)N - 2; i >= 0; --i)
    emit(Scry::GROW, {(int64_t)((Value >> (8 * i)) & 0xff)});
}

/// Emit the chain producing a symbol address: four instructions for the
/// absolute address, or three for the PC-relative one (a signed i32 whose
/// top byte is sign-extended, so symbols within 8 MB of the consumer fit;
/// the consumer's address is subtracted by relocateGlued once known).
void ScryTemporalLowering::emitSymAddr(const MachineOperand &MO, bool PCRel) {
  for (int B = PCRel ? 2 : 3; B >= 0; --B) {
    int Idx = (PCRel ? B == 2 : B == 3)
                  ? emit(Scry::CONST,
                         {(int64_t)(PCRel ? Scry::T_I32 : Scry::T_U32), 0})
                  : emit(Scry::GROW, {0});
    Item &I = Cur->Items[Idx];
    I.SymByte = B;
    I.PCRel = PCRel;
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
      report_fatal_error("Scry: unexpected symbol operand");
  }
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
/// delivered to the next instruction (or to the reference of the final add).
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
// Values in flight
//===----------------------------------------------------------------------===//

/// Number of operand uses of R in the current block after Def and before the
/// next definition of R in the block.
unsigned ScryTemporalLowering::countUsesAfter(Register R,
                                              const MachineInstr &Def) const {
  unsigned N = 0;
  bool After = false;
  for (const MachineInstr &MI : *CurMBB) {
    if (&MI == &Def) {
      After = true;
      continue;
    }
    if (!After || MI.isDebugInstr())
      continue;
    bool Redefines = false;
    for (const MachineOperand &MO : MI.operands()) {
      if (!MO.isReg() || MO.getReg() != R)
        continue;
      if (MO.isDef())
        Redefines = true;
      else if (MI.getOpcode() != TargetOpcode::KILL)
        ++N;
    }
    if (Redefines)
      break;
  }
  return N;
}

/// Number of operand uses of R in the current block before its first
/// definition there (uses of the value arriving at the block start).
unsigned ScryTemporalLowering::countUsesFromStart(Register R) const {
  unsigned N = 0;
  for (const MachineInstr &MI : *CurMBB) {
    if (MI.isDebugInstr())
      continue;
    bool Redefines = false;
    for (const MachineOperand &MO : MI.operands()) {
      if (!MO.isReg() || MO.getReg() != R)
        continue;
      if (MO.isDef())
        Redefines = true;
      else if (MI.getOpcode() != TargetOpcode::KILL)
        ++N;
    }
    if (Redefines)
      break;
  }
  return N;
}

/// Is R defined again in the current block after instruction After (or
/// anywhere in it when After is null)?
bool ScryTemporalLowering::definedLaterInBlock(Register R,
                                               const MachineInstr *After) const {
  bool Seen = After == nullptr;
  for (const MachineInstr &MI : *CurMBB) {
    if (&MI == After) {
      Seen = true;
      continue;
    }
    if (Seen && MI.definesRegister(R, /*TRI=*/nullptr))
      return true;
  }
  return false;
}

/// Execution time of an output for operand ordering: outputs of one item are
/// ordered by the way the item pushes them (echo pushes its second reference
/// first, dup its first).
int ScryTemporalLowering::outputTime(const Output &O) const {
  const Item &I = Cur->Items[O.Item];
  int Sub = O.RefOp < 0 ? 2 : O.RefOp;
  if (I.Opc == Scry::ECHO || I.Opc == Scry::ECHOS)
    Sub = 1 - Sub;
  return O.Item * 4 + Sub;
}

void ScryTemporalLowering::bindOutput(const Output &O, int Consumer,
                                      bool IsArg) {
  Item &I = Cur->Items[O.Item];
  if (O.RefOp < 0) {
    assert(Consumer == O.Item + 1 && "next-only output not glued");
    return;
  }
  assert(Cur->refValue(O.Item, Consumer, IsArg) <= O.Reach &&
         "reference out of reach");
  if (O.RefOp == 0) {
    I.RefOp = 0;
    I.RefTarget = Consumer;
    I.RefArg = IsArg;
  } else {
    I.Ref2Op = O.RefOp;
    I.Ref2Target = Consumer;
    I.Ref2Arg = IsArg;
  }
}

/// Register an unbound output of item ItemIdx carrying value V.  Dead values
/// are discarded by a nop; next-only outputs are captured by an echo; values
/// with several consumers are copied with dup right away.
void ScryTemporalLowering::produce(unsigned V, int ItemIdx, int RefOp,
                                   int Reach) {
  Output O{ItemIdx, RefOp, Reach};
  if (ItemIdx + 1 == curIndex() && RefOp >= 0)
    expire(3); // room for a dup or nop after the producer
  ValueState &VS = Values[V];
  if (VS.RemainingUses == 0) {
    int N = emit(Scry::NOP);
    bindOutput(O, N);
    return;
  }
  if (VS.RemainingUses >= 2) {
    int D = emit(Scry::DUP, {0, 0});
    bindOutput(O, D);
    ++FnDups;
    VS.Pending.push_back({D, 0, 31});
    VS.Pending.push_back({D, 1, 31});
    return;
  }
  if (RefOp < 0) {
    int H = emit(Scry::ECHOL, {0});
    bindOutput(O, H);
    ++FnHops;
    VS.Pending.push_back({H, 0, 1023});
    return;
  }
  VS.Pending.push_back(O);
}

/// Re-emit pending output Idx of V with an echo so that it can reach further.
void ScryTemporalLowering::hopValue(unsigned V, unsigned Idx) {
  ValueState &VS = Values[V];
  Output O = VS.Pending[Idx];
  int H = emit(Scry::ECHOL, {0});
  bindOutput(O, H);
  ++FnHops;
  VS.Pending[Idx] = {H, 0, 1023};
}

/// Make sure V has at least Count pending outputs, copying with dup.
void ScryTemporalLowering::ensureOutputs(unsigned V, unsigned Count) {
  ValueState &VS = Values[V];
  while (VS.Pending.size() < Count) {
    assert(!VS.Pending.empty() && "value without any output");
    Output O = VS.Pending.back();
    VS.Pending.pop_back();
    int D = emit(Scry::DUP, {0, 0});
    bindOutput(O, D);
    ++FnDups;
    VS.Pending.push_back({D, 0, 31});
    VS.Pending.push_back({D, 1, 31});
  }
}

/// Take the pending output of V with the smallest time greater than After
/// (or the smallest overall) and remove it from the pending list.
int ScryTemporalLowering::takeOutput(unsigned V, int After, Output &O) {
  ValueState &VS = Values[V];
  assert(!VS.Pending.empty() && "value without any output");
  int Best = -1, BestTime = 0;
  int Any = -1, AnyTime = 0;
  for (unsigned i = 0, e = VS.Pending.size(); i != e; ++i) {
    int T = outputTime(VS.Pending[i]);
    if (T > After && (Best < 0 || T < BestTime)) {
      Best = i;
      BestTime = T;
    }
    if (Any < 0 || T < AnyTime) {
      Any = i;
      AnyTime = T;
    }
  }
  int Pick = Best >= 0 ? Best : Any;
  O = VS.Pending[Pick];
  VS.Pending.erase(VS.Pending.begin() + Pick);
  return Best >= 0 ? BestTime : AnyTime;
}

/// Ensure V has a pending output that may be bound directly by the next
/// consumer: one more than the other remaining uses need.
void ScryTemporalLowering::ensureDirect(unsigned V) {
  ValueState &VS = Values[V];
  if (VS.RemainingUses > VS.Pending.size())
    ensureOutputs(V, VS.Pending.size() + 1);
}

/// Hop every pending output that could not reach an instruction Lookahead
/// items from here (with ExtraCalls calls emitted in between).  The margin
/// also covers one hop per pending output, so that outputs expiring together
/// can all be hopped in time; hops are emitted earliest deadline first.
void ScryTemporalLowering::expire(unsigned Lookahead, unsigned ExtraCalls) {
  // Every hop emitted here pushes the next Lookahead items one further, so
  // an output is due when it cannot reach the item Lookahead ahead even
  // after the hops of the outputs due before it.  Iterate to a fixed
  // point: hopping one output can only push others past their reach.
  // (deadline, value, output index)
  SmallVector<std::tuple<int, unsigned, unsigned>, 8> Due;
  unsigned Hops = 0;
  if (ExpireAllPending)
    for (const ValueState &VS : Values)
      Hops += VS.Pending.size();
  for (;;) {
    Due.clear();
    int P = curIndex() + (int)Lookahead + (int)Hops - 1;
    for (unsigned V = 0, e = Values.size(); V != e; ++V) {
      ValueState &VS = Values[V];
      for (unsigned i = 0, n = VS.Pending.size(); i != n; ++i) {
        const Output &O = VS.Pending[i];
        if (O.RefOp < 0)
          continue;
        if (Cur->refValue(O.Item, P, false) + (int)ExtraCalls > O.Reach)
          Due.push_back({O.Item + O.Reach, V, i});
      }
    }
    if (ExpireAllPending || Due.size() == Hops)
      break;
    Hops = Due.size();
  }
  llvm::sort(Due);
  for (auto &[D, V, I] : Due)
    hopValue(V, I);
}

unsigned ScryTemporalLowering::chainLength(const Operand &O) const {
  switch (O.K) {
  case Operand::Flight:
    return 0;
  case Operand::Slot:
    return shortSlot(O.Reg) ? 1 : frameAddrLen(slotOffset(O.Reg)) + 1;
  case Operand::Const:
    return constBytesLen(O.Imm, O.Scale, O.TypeCode & 1);
  case Operand::FrameAddr:
    return frameAddrLen(O.Imm);
  case Operand::SymAddr:
    return O.RelBase ? 3 : 4;
  }
  llvm_unreachable("bad operand kind");
}

/// Does the chain forward values aimed at its first instruction behind its
/// own result (const/grow), or discard them (ld.s, saddr)?
bool ScryTemporalLowering::chainPassesThrough(const Operand &O) const {
  return O.K == Operand::Const || O.K == Operand::SymAddr;
}

/// Immediate index of the reference of the chain's last instruction, or -1
/// when the chain delivers to the next instruction only.
int ScryTemporalLowering::chainRefOp(const Operand &O) const {
  switch (O.K) {
  case Operand::Slot:
    return shortSlot(O.Reg) ? -1 : 1; // ld T, =>r
  case Operand::FrameAddr: {
    unsigned S, I;
    return saddrForm(O.Imm, S, I) ? -1 : 0; // add Low =>r
  }
  default:
    return -1;
  }
}

/// Emit the producer chain of a non-flight operand.  Returns the index of
/// its first item and sets LastItem to its last.
int ScryTemporalLowering::emitChain(const Operand &O, int &LastItem,
                                    bool Glued) {
  int First = curIndex();
  switch (O.K) {
  case Operand::Flight:
    llvm_unreachable("not a chain");
  case Operand::Slot:
    ++FnSlotLoads;
    if (shortSlot(O.Reg)) {
      emit(Scry::LDS, {(int64_t)utype(O.Reg),
                       (int64_t)(slotOffset(O.Reg) / scaleOf(O.Reg))});
    } else {
      emitFrameAddr(slotOffset(O.Reg));
      emit(Scry::LD, {(int64_t)utype(O.Reg), 0});
    }
    break;
  case Operand::Const:
    emitConstBytes(O.TypeCode, O.Imm, O.Scale);
    break;
  case Operand::FrameAddr:
    emitFrameAddr(O.Imm);
    break;
  case Operand::SymAddr:
    emitSymAddr(*O.Sym, Glued && O.RelBase);
    break;
  }
  LastItem = curIndex() - 1;
  return First;
}

/// The items from First on were glued in front of the consumer at index
/// Consumer: make their PC-relative symbol bytes relative to it.
void ScryTemporalLowering::relocateGlued(int First, int Consumer) {
  for (int Idx = First; Idx < Consumer; ++Idx) {
    Item &I = Cur->Items[Idx];
    if (I.PCRel)
      I.SymOffset -= 2 * (int64_t)(Consumer - Idx);
  }
}

/// Emit whatever is needed so that the operands Ops arrive, in order, at the
/// instruction Gap items after the last item emitted here, and bind the
/// references accordingly.  Returns the index of that consumer position.
///
/// Operands in flight whose last emitters already execute in operand order
/// are aimed at the consumer directly.  The remaining operands are re-emitted
/// in order right before the consumer: an `echo` hop per run of in-flight
/// values, a captured `const`/`ld.s` chain per group of constants, and, with
/// Gap == 0, a trailing group of constants glued to the consumer (in-flight
/// values that must follow it are aimed at the chain, which forwards them).
int ScryTemporalLowering::prepareOperands(ArrayRef<Operand> Ops, unsigned Gap,
                                          bool IsArg) {
  // Every pending output must survive the prelude: hop those that cannot.
  // Done before and after the copies below, which emit instructions too.
  unsigned Look = Gap + 3;
  for (const Operand &O : Ops)
    Look += chainLength(O) + 2;
  expire(Look, IsArg ? 1 : 0);

  // Copies for values used by several operands or needed again later.
  SmallVector<unsigned, 4> Seen;
  for (const Operand &O : Ops) {
    if (O.K != Operand::Flight || llvm::is_contained(Seen, O.V))
      continue;
    Seen.push_back(O.V);
    unsigned Here = llvm::count_if(
        Ops, [&](const Operand &P) { return P.K == Operand::Flight && P.V == O.V; });
    ValueState &VS = Values[O.V];
    assert(VS.RemainingUses >= Here && "more uses than counted");
    ensureOutputs(O.V, Here + (VS.RemainingUses > Here ? 1 : 0));
  }

  expire(Look, IsArg ? 1 : 0);

  // Pick an output for every in-flight operand, preferring increasing times.
  unsigned N = Ops.size();
  SmallVector<Output, 4> Chosen(N);
  SmallVector<int, 4> Times(N, -1);
  int Prev = -1;
  for (unsigned i = 0; i != N; ++i) {
    if (Ops[i].K != Operand::Flight)
      continue;
    Times[i] = takeOutput(Ops[i].V, Prev, Chosen[i]);
    Prev = Times[i];
  }
  for (const Operand &O : Ops)
    if (O.K == Operand::Flight)
      --Values[O.V].RemainingUses;

  // Direct prefix: in-flight operands with increasing times.
  unsigned P = 0;
  Prev = -1;
  while (P != N && Ops[P].K == Operand::Flight && Times[P] > Prev) {
    Prev = Times[P];
    ++P;
  }

  // Plan A: prefix, a run of forwarding chains, and in-flight operands with
  // increasing times that ride through the chains.
  if (Gap == 0 && P != N && Ops[P].isChain()) {
    unsigned R = P;
    while (R != N && Ops[R].isChain() &&
           (chainPassesThrough(Ops[R]) || R + 1 == N))
      ++R;
    unsigned Q = R;
    if (R == N || (llvm::all_of(Ops.slice(P, R - P), [&](const Operand &O) {
                     return chainPassesThrough(O);
                   }))) {
      int QPrev = -1;
      while (Q != N && Ops[Q].K == Operand::Flight && Times[Q] > QPrev) {
        QPrev = Times[Q];
        ++Q;
      }
    }
    if (Q == N) {
      // The chains are emitted in reverse operand order: each const prepends
      // its value to what the previous chains delivered.  The riders are
      // aimed at the first chain emitted (the last operand of the run) and
      // are forwarded behind every chain's value.
      int RunStart = curIndex();
      int Last = -1;
      for (unsigned i = R; i-- > P;)
        emitChain(Ops[i], Last, /*Glued=*/true);
      int C = curIndex();
      relocateGlued(RunStart, C);
      for (unsigned i = 0; i != P; ++i)
        bindOutput(Chosen[i], C, IsArg);
      for (unsigned i = R; i != Q; ++i)
        bindOutput(Chosen[i], RunStart);
      return C;
      // (never deferred: Plan A needs Gap == 0)
    }
  }

  // Plan B: the suffix is re-emitted in order.
  SmallVector<std::pair<int, int>, 4> ToBind; // (item, refop) -> consumer
  unsigned i = P;
  while (i != N) {
    if (Ops[i].K == Operand::Flight) {
      // A decreasing pair is swapped by one echo; otherwise an increasing
      // run is carried by one echo.l.
      if (i + 1 < N && Ops[i + 1].K == Operand::Flight &&
          Times[i + 1] < Times[i] &&
          !(i + 2 < N && Ops[i + 2].K == Operand::Flight &&
            Times[i + 2] > Times[i + 1])) {
        int H = emit(Scry::ECHO, {0, 0});
        bindOutput(Chosen[i], H);
        bindOutput(Chosen[i + 1], H);
        ++FnHops;
        ToBind.push_back({H, 0});
        ToBind.push_back({H, 1});
        i += 2;
        continue;
      }
      unsigned j = i + 1;
      while (j != N && Ops[j].K == Operand::Flight && Times[j] > Times[j - 1])
        ++j;
      int H = emit(Scry::ECHOL, {0});
      for (unsigned k = i; k != j; ++k)
        bindOutput(Chosen[k], H);
      ++FnHops;
      ToBind.push_back({H, 0});
      i = j;
      continue;
    }
    // A chain followed by one last in-flight operand: the chain delivers to
    // an echo that the in-flight value is aimed at; since the value arrives
    // first, the echo's swap puts the chain's result in front.
    if (i + 2 == N && Ops[i + 1].K == Operand::Flight) {
      int Last;
      emitChain(Ops[i], Last);
      int H = emit(Scry::ECHO, {0, 0});
      bindOutput(Chosen[i + 1], H);
      ++FnHops;
      ToBind.push_back({H, 0});
      ToBind.push_back({H, 1});
      i += 2;
      continue;
    }
    // A group of chains: all but the last operand of the group forward.
    unsigned j = i;
    while (j != N && Ops[j].isChain() && (j == i || chainPassesThrough(Ops[j - 1])))
      ++j;
    bool AllPass = llvm::all_of(Ops.slice(i, j - i), [&](const Operand &O) {
      return chainPassesThrough(O);
    });
    // In-flight riders with increasing times may follow a forwarding group.
    unsigned q = j;
    if (AllPass) {
      int QPrev = -1;
      while (q != N && Ops[q].K == Operand::Flight && Times[q] > QPrev) {
        QPrev = Times[q];
        ++q;
      }
    }
    bool Trailing = q == N && Gap == 0;
    bool SingleRef = j == i + 1 && q == j && chainRefOp(Ops[i]) >= 0;
    int RunStart = curIndex();
    int Last = -1;
    for (unsigned k = j; k-- > i;)
      emitChain(Ops[k], Last);
    for (unsigned k = j; k != q; ++k)
      bindOutput(Chosen[k], RunStart);
    if (Trailing) {
      // Glued: the last chain delivers to the consumer directly.
      int C = curIndex() + Gap;
      for (unsigned k = 0; k != P; ++k)
        bindOutput(Chosen[k], C, IsArg);
      for (auto &B : ToBind)
        bindOutput({B.first, B.second, 1023}, C, IsArg);
      return C;
    }
    if (SingleRef) {
      // The chain's last instruction has a reference of its own.
      ToBind.push_back({Last, chainRefOp(Ops[i])});
    } else {
      int H = emit(Scry::ECHOL, {0});
      ++FnHops;
      ToBind.push_back({H, 0});
    }
    i = q;
  }

  int C = curIndex() + Gap;
  if (DeferredBinds) {
    for (unsigned k = 0; k != P; ++k)
      DeferredBinds->push_back({Chosen[k], 0});
    for (auto &B : ToBind)
      DeferredBinds->push_back({Output{B.first, B.second, 1023}, 0});
    return C;
  }
  for (unsigned k = 0; k != P; ++k)
    bindOutput(Chosen[k], C, IsArg);
  for (auto &B : ToBind)
    bindOutput({B.first, B.second, 1023}, C, IsArg);
  return C;
}

int ScryTemporalLowering::consume(unsigned Opc, ArrayRef<Operand> Ops,
                                  std::initializer_list<int64_t> Imms) {
  int C = prepareOperands(Ops, 0);
  int Idx = emit(Opc, Imms);
  assert(Idx == C && "consumer not at the prepared position");
  (void)C;
  return Idx;
}

/// Time of the earliest pending output of an in-flight operand (chains are
/// built at the consumer and come last).
int ScryTemporalLowering::nextOutputTime(const Operand &O) const {
  if (O.K != Operand::Flight)
    return INT32_MAX;
  const ValueState &VS = Values[O.V];
  // A value that must be copied first is re-emitted by the dup, i.e. late.
  if (VS.RemainingUses > VS.Pending.size())
    return INT32_MAX - 1;
  int T = INT32_MAX;
  for (const Output &P : VS.Pending)
    T = std::min(T, outputTime(P));
  return T;
}

/// A commutative two-input operation: order the operands so that they are
/// already in order and no hop is needed.
int ScryTemporalLowering::consumeCommutative(unsigned Opc, Operand A,
                                             Operand B,
                                             std::initializer_list<int64_t> Imms) {
  bool Swap = false;
  if (A.K == Operand::Flight && B.K == Operand::Flight) {
    // Both in flight: the one produced earlier goes first.
    Swap = nextOutputTime(B) < nextOutputTime(A) &&
           !(A.V == B.V);
  } else if (A.K != Operand::Flight && B.K == Operand::Flight) {
    // [chain, value] needs the value to ride through the chain; fine for
    // forwarding chains, otherwise put the value first.
    Swap = !chainPassesThrough(A);
  }
  if (Swap)
    return consume(Opc, {B, A}, Imms);
  return consume(Opc, {A, B}, Imms);
}

/// Store the value delivered by Src into the slot of R.
void ScryTemporalLowering::storeOperand(Register R, const Operand &Src) {
  ++FnSlotStores;
  if (shortSlot(R)) {
    consume(Scry::STS, {Src}, {(int64_t)(slotOffset(R) / scaleOf(R))});
    return;
  }
  consume(Scry::ST, {Src, Operand::frameAddr(slotOffset(R))});
}

/// Store value V into the slot of R.
void ScryTemporalLowering::storeToSlot(Register R, unsigned V) {
  storeOperand(R, Operand::flight(V));
}

/// The operand delivering the value of register use MO.
Operand ScryTemporalLowering::operandFor(const MachineOperand &MO) {
  Register R = MO.getReg();
  auto It = RegValue.find(R);
  if (It != RegValue.end())
    return Operand::flight(It->second);
  auto RI = Remat.find(R);
  if (RI != Remat.end()) {
    const MachineInstr &Def = *RI->second;
    switch (Def.getOpcode()) {
    case Scry::CONST_8: case Scry::CONST_16: case Scry::CONST_32: case Scry::CONST_64: {
      unsigned Scale = scaleOf(R);
      uint64_t Mask = Scale == 8 ? ~0ull : ((1ull << (8 * Scale)) - 1);
      return Operand::constant(utype(R), (uint64_t)Def.getOperand(1).getImm() & Mask,
                               Scale);
    }
    case Scry::ADDR_SYM:
      return Operand::symAddr(&Def.getOperand(1));
    case Scry::ADDR_FI:
      return Operand::frameAddr((unsigned)Def.getOperand(1).getImm());
    case Scry::ADDR_OUT:
      return Operand::frameAddr(PrivSize + (unsigned)Def.getOperand(1).getImm());
    default:
      llvm_unreachable("bad rematerialisable definition");
    }
  }
  auto AI = SlotAlias.find(R);
  if (AI != SlotAlias.end())
    return Operand::slot(AI->second);
  if (hasSlot(R)) {
    // A slot value the block uses again (before redefining it) is loaded
    // once and kept in flight: a load plus a dup instead of a load per use.
    if (CurExpandMI) {
      unsigned Uses = 0;
      for (const MachineOperand &O : CurExpandMI->operands())
        if (O.isReg() && O.getReg() == R && O.isUse())
          ++Uses;
      Uses += countUsesAfter(R, *CurExpandMI);
      if (Uses >= 2) {
        Operand Src = Operand::slot(R);
        expire(chainLength(Src) + 3);
        int Last;
        emitChain(Src, Last);
        unsigned V = newValue(Uses);
        RegValue[R] = V;
        int RefOp = chainRefOp(Src);
        produce(V, Last, RefOp, RefOp >= 0 ? 31 : 0);
        return Operand::flight(V);
      }
    }
    return Operand::slot(R);
  }
  // A constant without a slot is rebuilt wherever it is used.  Tail
  // duplication may have copied its definition into several blocks; the
  // copies are identical, so any of them serves.
  const MachineInstr *Def = nullptr;
  for (const MachineInstr &D : MRI->def_instructions(R)) {
    if (Def && !D.isIdenticalTo(*Def)) {
      Def = nullptr;
      break;
    }
    Def = &D;
  }
  if (Def) {
    switch (Def->getOpcode()) {
    case Scry::CONST_8: case Scry::CONST_16: case Scry::CONST_32: case Scry::CONST_64: {
      unsigned Scale = scaleOf(R);
      uint64_t Mask = Scale == 8 ? ~0ull : ((1ull << (8 * Scale)) - 1);
      return Operand::constant(utype(R), (uint64_t)Def->getOperand(1).getImm() & Mask,
                               Scale);
    }
    case Scry::ADDR_SYM:
      return Operand::symAddr(&Def->getOperand(1));
    case Scry::ADDR_FI:
      return Operand::frameAddr((unsigned)Def->getOperand(1).getImm());
    case Scry::ADDR_OUT:
      return Operand::frameAddr(PrivSize + (unsigned)Def->getOperand(1).getImm());
    default:
      break;
    }
  }
  // Undefined (IMPLICIT_DEF) or otherwise unavailable: a zero.
  return Operand::constant(utype(R), 0, scaleOf(R));
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

  // Index of the instruction defining R if it is in MBB, else -1 (the jump
  // is issued at the block start, after the prologue).
  auto boundaryAfterDef = [&](Register R) -> unsigned {
    // A register may be defined in several blocks after tail duplication;
    // only a definition in this block matters.
    for (MachineInstr &Def : MRI->def_instructions(R))
      if (Def.getParent() == &MBB)
        return MIIndex.lookup(&Def) + 1;
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

static bool isOrderingCompare(unsigned Opc) {
  switch (Opc) {
  case Scry::CMP_ULT_GR8: case Scry::CMP_ULT_GR16: case Scry::CMP_ULT_GR32: case Scry::CMP_ULT_GR64:
  case Scry::CMP_UGT_GR8: case Scry::CMP_UGT_GR16: case Scry::CMP_UGT_GR32: case Scry::CMP_UGT_GR64:
  case Scry::CMP_SLT_GR8: case Scry::CMP_SLT_GR16: case Scry::CMP_SLT_GR32: case Scry::CMP_SLT_GR64:
  case Scry::CMP_SGT_GR8: case Scry::CMP_SGT_GR16: case Scry::CMP_SGT_GR32: case Scry::CMP_SGT_GR64:
    return true;
  default:
    return false;
  }
}

static bool isEqualityCompare(unsigned Opc) {
  switch (Opc) {
  case Scry::CMP_EQ_GR8: case Scry::CMP_EQ_GR16: case Scry::CMP_EQ_GR32: case Scry::CMP_EQ_GR64:
  case Scry::CMP_NE_GR8: case Scry::CMP_NE_GR16: case Scry::CMP_NE_GR32: case Scry::CMP_NE_GR64:
    return true;
  default:
    return false;
  }
}

/// A jump that must negate its condition and is the only user of a compare
/// takes the compare's result in the needed polarity instead: a negating
/// compare drops its final eq, an equality compare becomes xor.
void ScryTemporalLowering::planNegationFolds(MachineBasicBlock &MBB,
                                             ArrayRef<PlannedJump> Jumps) {
  for (const PlannedJump &J : Jumps) {
    if (!J.Cond.isValid())
      continue;
    MachineInstr *Def = MRI->hasOneDef(J.Cond) ? MRI->getVRegDef(J.Cond) : nullptr;
    if (!Def || Def->getParent() != &MBB || !MRI->hasOneNonDBGUse(J.Cond) ||
        !(isNegatingCompare(Def->getOpcode()) ||
          isEqualityCompare(Def->getOpcode()) ||
          isOrderingCompare(Def->getOpcode())))
      continue;
    JumpOnlyCompares.insert(Def);
    JumpOnlyInfo[Def] = {isBackward(&MBB, J.Target), J.TakenOnNonZero};
    if (jumpNeedsNegation(MBB, J) && isNegatingCompare(Def->getOpcode()) &&
        !isEqualityCompare(Def->getOpcode()))
      FoldedNegations.insert(Def);
  }
}

/// Emit jump J here.  Forms:
///   near, unconditional:  jmp t, trig
///   near, conditional:    [cond] ; [eq =>0] ; jmp t, trig
///   far, unconditional:   const i8/i16/i32 t (grow ...) ; const u8 pol ; jmp
///   far, conditional:     [cond aimed past the chain] ; const ... ; jmp
/// The condition is the jmp's first operand; the far forms deliver the target
/// as the second operand (const forwards the condition behind its own value,
/// so the polarity constant of an unconditional far jump goes last).
void ScryTemporalLowering::emitJump(MachineBasicBlock &MBB,
                                    const PlannedJump &J) {
  JumpForm Form = JumpForms.lookup(J.Key);
  unsigned ChainLen =
      Form == JF_Near ? 0 : Form == JF_Far8 ? 1 : Form == JF_Far16 ? 2 : 4;
  expire(ChainLen + 6);
  int First = curIndex();

  if (J.Cond.isValid()) {
    // Equality compares feeding only this jump produce the operand in the
    // needed polarity themselves; so do folded negating compares.
    const MachineInstr *CondDef =
        MRI->hasOneDef(J.Cond) ? MRI->getVRegDef(J.Cond) : nullptr;
    bool Handled = CondDef && (FoldedNegations.count(CondDef) ||
                               (JumpOnlyCompares.count(CondDef) &&
                                isEqualityCompare(CondDef->getOpcode())));
    bool Neg = jumpNeedsNegation(MBB, J) && !Handled;
    Operand C = operandFor(MachineOperand::CreateReg(J.Cond, false));
    if (Neg) {
      ++FnNegations;
      unsigned T = newValue(1);
      int E = consume(Scry::EQ, {C}, {0});
      produce(T, E, 0, 31);
      C = Operand::flight(T);
    }
    int Pos = prepareOperands({C}, ChainLen);
    (void)Pos;
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

  raw_string_ostream OS(Cur->Items[First].Comment);
  OS << "jump"
     << (J.Cond.isValid() ? (J.TakenOnNonZero ? " if non-zero" : " if zero")
                          : "")
     << " -> " << printMBBReference(*J.Target);
}

//===----------------------------------------------------------------------===//
// Expansion of individual instructions
//===----------------------------------------------------------------------===//

/// Create the value for the result register D of MI, counting its consumers
/// in this block (plus the slot store if it lives across blocks).
unsigned ScryTemporalLowering::defineValue(Register D, const MachineInstr &MI) {
  unsigned Uses = countUsesAfter(D, MI) + extraUses(D, &MI);
  unsigned V = newValue(Uses);
  RegValue[D] = V;
  SlotAlias.erase(D);
  Remat.erase(D);
  return V;
}

/// Register the output of the last emitted item as MI's result, storing it
/// to the result's slot when it lives across blocks.
void ScryTemporalLowering::defineResult(MachineInstr &MI, unsigned V) {
  Register D = MI.getOperand(0).getReg();
  if (hasSlot(D))
    storeToSlot(D, V);
}

void ScryTemporalLowering::expandBinary(MachineInstr &MI, unsigned RealOpc) {
  Register D = MI.getOperand(0).getReg();
  Operand A = operandFor(MI.getOperand(1));
  Operand B = operandFor(MI.getOperand(2));
  unsigned V = defineValue(D, MI);
  bool Commutative = RealOpc == Scry::ADD_L || RealOpc == Scry::MUL_L ||
                     RealOpc == Scry::AND || RealOpc == Scry::OR ||
                     RealOpc == Scry::XOR;
  // A constant whose upper bytes are a sign extension is shorter as a signed
  // constant (const sign-extends its byte).  The result keeps the other
  // operand's unsigned type, so this is only done when that operand is a
  // value (values are tagged unsigned) and the operation does not depend on
  // the signedness of its inputs.
  bool TypeNeutral = RealOpc == Scry::ADD_L || RealOpc == Scry::SUB_L ||
                     RealOpc == Scry::MUL_L || RealOpc == Scry::AND ||
                     RealOpc == Scry::OR || RealOpc == Scry::XOR;
  auto Shorten = [&](Operand &C, const Operand &Other) {
    if (!TypeNeutral || C.K != Operand::Const || (C.TypeCode & 1) ||
        Other.K == Operand::Const)
      return;
    if (constBytesLen(C.Imm, C.Scale, true) < constBytesLen(C.Imm, C.Scale, false))
      C.TypeCode |= 1;
  };
  Shorten(A, B);
  Shorten(B, A);
  // The implicit second operand of a single-input operation: 1 for add,
  // sub, shift and and; all ones for or and xor; 2 for div.
  auto IsImplicit = [&](const Operand &C) {
    if (C.K != Operand::Const)
      return false;
    uint64_t Mask = C.Scale == 8 ? ~0ull : ((1ull << (8 * C.Scale)) - 1);
    uint64_t Val = C.Imm & Mask;
    switch (RealOpc) {
    case Scry::ADD_L: case Scry::SUB_L: case Scry::SH_L: case Scry::AND:
      return Val == 1;
    case Scry::OR: case Scry::XOR:
      return Val == Mask;
    case Scry::DIV_L: case Scry::DIV_H:
      return Val == 2;
    default:
      return false;
    }
  };
  // x + (-1) is the single-input sub, x - (-1) the single-input add.
  auto IsAllOnes = [&](const Operand &C) {
    if (C.K != Operand::Const)
      return false;
    uint64_t Mask = C.Scale == 8 ? ~0ull : ((1ull << (8 * C.Scale)) - 1);
    return (C.Imm & Mask) == Mask;
  };
  if (RealOpc == Scry::ADD_L || RealOpc == Scry::SUB_L) {
    unsigned Flipped = RealOpc == Scry::ADD_L ? Scry::SUB_L : Scry::ADD_L;
    if (IsAllOnes(B)) {
      int Op = consume(Flipped, {A}, {0});
      produce(V, Op, 0, 31);
      defineResult(MI, V);
      return;
    }
    if (Commutative && IsAllOnes(A)) {
      int Op = consume(Flipped, {B}, {0});
      produce(V, Op, 0, 31);
      defineResult(MI, V);
      return;
    }
  }
  if (IsImplicit(B)) {
    int Op = consume(RealOpc, {A}, {0});
    produce(V, Op, 0, 31);
    defineResult(MI, V);
    return;
  }
  if (Commutative && IsImplicit(A)) {
    int Op = consume(RealOpc, {B}, {0});
    produce(V, Op, 0, 31);
    defineResult(MI, V);
    return;
  }
  int Op = Commutative ? consumeCommutative(RealOpc, A, B, {0})
                       : consume(RealOpc, {A, B}, {0});
  produce(V, Op, 0, 31);
  defineResult(MI, V);
}

/// Logical / arithmetic right shift: the shift amount must be a negative
/// signed value, so compute 0 - n (as i32).
/// A shift's result has the type of its first operand, so an arithmetic
/// shift (whose operand is cast signed) produces a signed-tagged value.
/// Same-width arithmetic treats it like any other (mixed signedness makes
/// both operands unsigned), but a widening cast would sign-extend it, a
/// compare against an unsigned value would use its signed logical value, a
/// logical shift would fill with the sign, and anything leaving the block
/// is beyond tracking.  Such uses need the tag put back by a cast.
static bool ashrResultNeedsUnsignedTag(const MachineInstr &MI,
                                       const MachineRegisterInfo &MRI) {
  Register D = MI.getOperand(0).getReg();
  for (const MachineInstr &Use : MRI.use_nodbg_instructions(D)) {
    if (Use.getParent() != MI.getParent())
      return true;
    switch (Use.getOpcode()) {
    case Scry::ADD_GR8: case Scry::ADD_GR16: case Scry::ADD_GR32: case Scry::ADD_GR64:
    case Scry::SUB_GR8: case Scry::SUB_GR16: case Scry::SUB_GR32: case Scry::SUB_GR64:
    case Scry::MUL_GR8: case Scry::MUL_GR16: case Scry::MUL_GR32: case Scry::MUL_GR64:
    case Scry::AND_GR8: case Scry::AND_GR16: case Scry::AND_GR32: case Scry::AND_GR64:
    case Scry::OR_GR8: case Scry::OR_GR16: case Scry::OR_GR32: case Scry::OR_GR64:
    case Scry::XOR_GR8: case Scry::XOR_GR16: case Scry::XOR_GR32: case Scry::XOR_GR64:
    case Scry::SEXT_8_16: case Scry::SEXT_8_32: case Scry::SEXT_8_64:
    case Scry::SEXT_16_32: case Scry::SEXT_16_64: case Scry::SEXT_32_64:
    case Scry::SEXTINREG_16: case Scry::SEXTINREG_32: case Scry::SEXTINREG_64:
    case Scry::TRUNC_16_8: case Scry::TRUNC_32_8: case Scry::TRUNC_64_8:
    case Scry::TRUNC_32_16: case Scry::TRUNC_64_16: case Scry::TRUNC_64_32:
    case Scry::ASHR_GR8: case Scry::ASHR_GR16: case Scry::ASHR_GR32: case Scry::ASHR_GR64:
    case Scry::CMP_SLT_GR8: case Scry::CMP_SLT_GR16: case Scry::CMP_SLT_GR32: case Scry::CMP_SLT_GR64:
    case Scry::CMP_SLE_GR8: case Scry::CMP_SLE_GR16: case Scry::CMP_SLE_GR32: case Scry::CMP_SLE_GR64:
    case Scry::CMP_SGT_GR8: case Scry::CMP_SGT_GR16: case Scry::CMP_SGT_GR32: case Scry::CMP_SGT_GR64:
    case Scry::CMP_SGE_GR8: case Scry::CMP_SGE_GR16: case Scry::CMP_SGE_GR32: case Scry::CMP_SGE_GR64:
      break;
    case Scry::ST_8: case Scry::ST_16: case Scry::ST_32: case Scry::ST_64:
    case Scry::STX_8: case Scry::STX_16: case Scry::STX_32: case Scry::STX_64:
      if (Use.getOperand(0).getReg() != D)
        return true; // as an address: zero-extended by the ISA, but keep it simple
      break;
    default:
      return true;
    }
  }
  return false;
}

void ScryTemporalLowering::expandRightShift(MachineInstr &MI,
                                            bool Arithmetic) {
  Register D = MI.getOperand(0).getReg();
  Register A = MI.getOperand(1).getReg();
  Operand ValOp = operandFor(MI.getOperand(1));
  Operand NOp = operandFor(MI.getOperand(2));
  unsigned V = defineValue(D, MI);

  // The amount as a negative i32: a constant directly, else 0 - n.
  Operand Neg;
  if (NOp.K == Operand::Const) {
    Neg = Operand::constant(Scry::T_I32, (uint64_t)(-(int64_t)NOp.Imm) & 0xffffffffu, 4);
  } else {
    unsigned TN = newValue(1);
    int Cast = consume(Scry::CAST, {NOp}, {(int64_t)Scry::T_I32, 0});
    produce(TN, Cast, 1, 31);
    unsigned TNeg = newValue(1);
    int Sub = consume(Scry::SUB_L,
                      {Operand::constant(Scry::T_I32, 0, 4), Operand::flight(TN)},
                      {0});
    produce(TNeg, Sub, 0, 31);
    Neg = Operand::flight(TNeg);
  }

  Operand Shifted = ValOp;
  if (Arithmetic) {
    unsigned TS = newValue(1);
    int SC = consume(Scry::CAST, {ValOp}, {(int64_t)stype(A), 0});
    produce(TS, SC, 1, 31);
    Shifted = Operand::flight(TS);
  }
  int Sh = consume(Scry::SH_L, {Shifted, Neg}, {0});
  if (Arithmetic && ashrResultNeedsUnsignedTag(MI, *MRI)) {
    unsigned TR = newValue(1);
    produce(TR, Sh, 0, 31);
    Sh = consume(Scry::CAST, {Operand::flight(TR)}, {(int64_t)utype(D), 0});
    produce(V, Sh, 1, 31);
  } else {
    produce(V, Sh, 0, 31);
  }
  defineResult(MI, V);
}

/// Comparison producing a u8 boolean: First is EQ/LT/GT, optionally negated
/// by a second eq (against the implicit 0).  A compare feeding only a jump
/// that wants the opposite polarity is folded: the negation is dropped, or
/// an equality becomes xor (zero exactly when equal).
void ScryTemporalLowering::expandCompare(MachineInstr &MI, unsigned First,
                                         bool Negate, bool Signed) {
  Register D = MI.getOperand(0).getReg();
  Register A = MI.getOperand(1).getReg();
  Register B = MI.getOperand(2).getReg();
  Operand OA = operandFor(MI.getOperand(1));
  Operand OB = operandFor(MI.getOperand(2));
  bool Folded = FoldedNegations.count(&MI);

  // An equality test feeding only a jump costs one instruction whatever the
  // polarities: the jump fires on a non-zero operand (backward target) or
  // on zero (forward), and the branch fires when a == b (eq) or a != b
  // (ne), so the operand is either "a == b" (eq) or "a != b" (xor, or the
  // value itself when b is zero).
  if (First == Scry::EQ && JumpOnlyCompares.count(&MI) && !Signed) {
    auto [FiresOnNonZero, Pol] = JumpOnlyInfo.lookup(&MI);
    bool BranchOnEqual = Pol != Negate;      // the branch fires when a == b
    bool WantNonZero = BranchOnEqual != FiresOnNonZero; // operand = (a != b)
    bool ZeroB = OB.K == Operand::Const && OB.Imm == 0;
    if (!ZeroB) {
      unsigned V = defineValue(D, MI);
      int C = WantNonZero ? consumeCommutative(Scry::XOR, OA, OB, {0})
                          : consumeCommutative(Scry::EQ, OA, OB, {0});
      produce(V, C, 0, 31);
      defineResult(MI, V);
      return;
    }
    if (WantNonZero && OA.K == Operand::Flight) {
      RegValue[D] = OA.V;
      SlotAlias.erase(D);
      Remat.erase(D);
      return;
    }
    if (WantNonZero && OA.K == Operand::Slot) {
      RegValue.erase(D);
      Remat.erase(D);
      SlotAlias[D] = OA.Reg;
      return;
    }
    if (!WantNonZero) {
      unsigned V = defineValue(D, MI);
      int E = consume(Scry::EQ, {OA}, {0});
      produce(V, E, 0, 31);
      defineResult(MI, V);
      return;
    }
  }

  unsigned V = defineValue(D, MI);
  if (Folded) {
    if (Negate)
      Negate = false;
    else if (First == Scry::EQ)
      First = Scry::XOR;
    else
      llvm_unreachable("fold of a non-negating compare");
  }

  // Comparing against zero: the implicit second operand is zero.
  bool ZeroB = OB.K == Operand::Const && OB.Imm == 0;
  if (Signed) {
    unsigned TA = newValue(1);
    int CA = consume(Scry::CAST, {OA}, {(int64_t)stype(A), 0});
    produce(TA, CA, 1, 31);
    OA = Operand::flight(TA);
    if (!ZeroB) {
      unsigned TB = newValue(1);
      int CB = consume(Scry::CAST, {OB}, {(int64_t)stype(B), 0});
      produce(TB, CB, 1, 31);
      OB = Operand::flight(TB);
    }
  }
  int Cmp = ZeroB ? consume(First, {OA}, {0}) : consume(First, {OA, OB}, {0});
  if (Negate) {
    unsigned T = newValue(1);
    produce(T, Cmp, 0, 31);
    Cmp = consume(Scry::EQ, {Operand::flight(T)}, {0});
  }
  produce(V, Cmp, 0, 31);
  defineResult(MI, V);
}

/// Apply a chain of casts to the source.
/// Is the use of the narrow value R by Use indifferent to R's width?  The
/// simulator gives a mixed-width operation the wider operand's width and
/// compares numerically, so a zero-extended value and its narrow original
/// behave the same as a compare operand, a shift amount, an index, or an
/// operand of an arithmetic or logic operation whose other operand is wide.
bool ScryTemporalLowering::widthTolerantUse(const MachineInstr &Use,
                                            Register R) const {
  // The other operand must really be wide: not a narrow value itself, and
  // not a constant the expansion folds into the implicit operand (which
  // takes the type of the single input), e.g. x + 1 as a one-input add.
  auto OtherIsWide = [&](unsigned A, unsigned B) {
    const MachineOperand &MA = Use.getOperand(A), &MB = Use.getOperand(B);
    if (!MA.isReg() || !MB.isReg())
      return false;
    Register Other = MA.getReg() == R ? MB.getReg() : MA.getReg();
    if (Other == R || NarrowRegs.count(Other))
      return false;
    const MachineInstr *Def = MRI->hasOneDef(Other) ? MRI->getVRegDef(Other) : nullptr;
    if (!Def)
      return false;
    switch (Def->getOpcode()) {
    case Scry::CONST_8: case Scry::CONST_16: case Scry::CONST_32: case Scry::CONST_64: {
      unsigned Scale = scaleOf(Other);
      uint64_t Mask = Scale == 8 ? ~0ull : ((1ull << (8 * Scale)) - 1);
      uint64_t Val = (uint64_t)Def->getOperand(1).getImm() & Mask;
      switch (Use.getOpcode()) {
      case Scry::ADD_GR16: case Scry::ADD_GR32: case Scry::ADD_GR64:
      case Scry::SUB_GR16: case Scry::SUB_GR32: case Scry::SUB_GR64:
      case Scry::AND_GR16: case Scry::AND_GR32: case Scry::AND_GR64:
        return Val != 1;
      case Scry::OR_GR16: case Scry::OR_GR32: case Scry::OR_GR64:
      case Scry::XOR_GR16: case Scry::XOR_GR32: case Scry::XOR_GR64:
        return Val != Mask;
      case Scry::UDIV_GR16: case Scry::UDIV_GR32: case Scry::UDIV_GR64:
      case Scry::UREM_GR16: case Scry::UREM_GR32: case Scry::UREM_GR64:
        return Val != 2;
      default:
        return true;
      }
    }
    default:
      return true;
    }
  };
  switch (Use.getOpcode()) {
  case Scry::ADD_GR16: case Scry::ADD_GR32: case Scry::ADD_GR64:
  case Scry::SUB_GR16: case Scry::SUB_GR32: case Scry::SUB_GR64:
  case Scry::MUL_GR16: case Scry::MUL_GR32: case Scry::MUL_GR64:
  case Scry::AND_GR16: case Scry::AND_GR32: case Scry::AND_GR64:
  case Scry::OR_GR16: case Scry::OR_GR32: case Scry::OR_GR64:
  case Scry::XOR_GR16: case Scry::XOR_GR32: case Scry::XOR_GR64:
  case Scry::UDIV_GR16: case Scry::UDIV_GR32: case Scry::UDIV_GR64:
  case Scry::UREM_GR16: case Scry::UREM_GR32: case Scry::UREM_GR64:
    return OtherIsWide(1, 2);
  case Scry::SHL_GR8: case Scry::SHL_GR16: case Scry::SHL_GR32: case Scry::SHL_GR64:
  case Scry::LSHR_GR8: case Scry::LSHR_GR16: case Scry::LSHR_GR32: case Scry::LSHR_GR64:
  case Scry::ASHR_GR8: case Scry::ASHR_GR16: case Scry::ASHR_GR32: case Scry::ASHR_GR64:
    // Only as the amount.
    return Use.getOperand(2).getReg() == R && Use.getOperand(1).getReg() != R;
  case Scry::LDX_8: case Scry::LDX_16: case Scry::LDX_32: case Scry::LDX_64:
    return Use.getOperand(2).getReg() == R && Use.getOperand(1).getReg() != R;
  case Scry::STX_8: case Scry::STX_16: case Scry::STX_32: case Scry::STX_64:
    return Use.getOperand(2).getReg() == R && Use.getOperand(0).getReg() != R &&
           Use.getOperand(1).getReg() != R;
  default:
    return isEqualityCompare(Use.getOpcode()) ||
           isOrderingCompare(Use.getOpcode()) ||
           isNegatingCompare(Use.getOpcode());
  }
}

/// A zero extension whose source is in flight and whose uses, all in this
/// block, are indifferent to the value's width is not emitted: the uses
/// take the narrow value.
bool ScryTemporalLowering::tryElideZext(MachineInstr &MI) {
  if (!ElideCasts || ElidedBefore + FnElidedCasts >= ElideCastsLimit)
    return false;
  Register D = MI.getOperand(0).getReg();
  Register S = MI.getOperand(1).getReg();
  if (hasSlot(D) || FuncInfo->isEdgeFlight(D) || DeliverSet.count(D) ||
      !MRI->hasOneDef(D))
    return false;
  auto It = RegValue.find(S);
  if (It == RegValue.end())
    return false;
  if (!MRI->hasOneDef(S))
    return false;
  for (const MachineInstr &Use : MRI->use_nodbg_instructions(D))
    if (Use.getParent() != CurMBB || !widthTolerantUse(Use, D))
      return false;
  unsigned V = It->second;
  Values[V].RemainingUses += countUsesAfter(D, MI) + extraUses(D, &MI);
  --Values[V].RemainingUses; // the cast's own use of S
  RegValue[D] = V;
  SlotAlias.erase(D);
  Remat.erase(D);
  NarrowRegs.insert(D);
  ++FnElidedCasts;
  return true;
}

void ScryTemporalLowering::expandCasts(MachineInstr &MI,
                                       ArrayRef<unsigned> TypeCodes) {
  Register D = MI.getOperand(0).getReg();
  Operand S = operandFor(MI.getOperand(1));
  unsigned V = defineValue(D, MI);
  int Last = -1;
  for (unsigned i = 0, e = TypeCodes.size(); i != e; ++i) {
    Last = consume(Scry::CAST, {S}, {(int64_t)TypeCodes[i], 0});
    if (i + 1 != e) {
      unsigned T = newValue(1);
      produce(T, Last, 1, 31);
      S = Operand::flight(T);
    }
  }
  produce(V, Last, 1, 31);
  defineResult(MI, V);
}

void ScryTemporalLowering::expandCall(MachineInstr &MI) {
  bool HasResult = MI.getNumExplicitDefs() == 1;
  unsigned CalleeIdx = HasResult ? 1 : 0;
  const MachineOperand &CalleeMO = MI.getOperand(CalleeIdx);

  SmallVector<Operand, 4> Args;
  for (unsigned i = CalleeIdx + 1, e = MI.getNumOperands(); i != e; ++i) {
    const MachineOperand &MO = MI.getOperand(i);
    if (MO.isReg() && !MO.isImplicit())
      Args.push_back(operandFor(MO));
  }

  Operand Callee = CalleeMO.isReg() ? operandFor(CalleeMO)
                                    : Operand::symAddr(&CalleeMO);
  Callee.RelBase = Callee.K == Operand::SymAddr;
  unsigned CalleeLen = Callee.K == Operand::Flight ? 0 : chainLength(Callee);
  if (Callee.K == Operand::Flight) {
    expire(3);
    ensureDirect(Callee.V);
  }

  // Arguments -> the instruction after the call; the callee address -> call.
  int ArgsPos = prepareOperands(Args, CalleeLen + 1, /*IsArg=*/true);
  int Call;
  if (Callee.K == Operand::Flight) {
    Output O;
    takeOutput(Callee.V, -1, O);
    --Values[Callee.V].RemainingUses;
    Call = emit(Scry::CALL, {0});
    bindOutput(O, Call);
  } else {
    int Last;
    int First = emitChain(Callee, Last, /*Glued=*/true);
    Call = emit(Scry::CALL, {0});
    relocateGlued(First, Call);
  }
  assert(Call + 1 == ArgsPos && "call trigger slot mismatch");
  (void)ArgsPos;
  Cur->Calls.push_back(Call);

  // The return value arrives at the trigger slot: capture it there, or
  // discard an unused one.  A void callee returns nothing, so the next
  // instruction can take the slot itself.
  if (HasResult) {
    Register D = MI.getOperand(0).getReg();
    unsigned V = defineValue(D, MI);
    if (Values[V].RemainingUses == 0) {
      emit(Scry::NOP);
    } else {
      produce(V, Call, -1, 0); // next-only: captured by an echo
      defineResult(MI, V);
    }
  }
}

void ScryTemporalLowering::expandReturn(MachineInstr &MI) {
  SmallVector<Operand, 4> Vals;
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && !MO.isImplicit())
      Vals.push_back(operandFor(MO));
  unsigned NumFrees = llvm::popcount(ReserveSize);
  int After = prepareOperands(Vals, NumFrees + 1);
  emitFrees();
  int Ret = emit(Scry::RET, {0});
  assert(Ret + 1 == After && "return value slot mismatch");
  (void)Ret;
  (void)After;
}

/// %d = COPY %s.  A copy of a value in flight shares the value; a copy of a
/// constant is rebuilt at its uses; a copy into a slot stores the source; a
/// copy of a slot value that is not stored again reads the source slot at
/// each use; anything else loads the value into flight.
void ScryTemporalLowering::expandCopy(MachineInstr &MI) {
  Register D = MI.getOperand(0).getReg();
  const MachineOperand &SMO = MI.getOperand(1);
  Register S = SMO.getReg();
  RegValue.erase(D);
  SlotAlias.erase(D);
  Remat.erase(D);

  Operand Src = operandFor(SMO);
  if (Src.K == Operand::Flight) {
    unsigned V = Src.V;
    // The copy's own use is replaced by the uses of D.
    Values[V].RemainingUses += countUsesAfter(D, MI) + extraUses(D, &MI);
    --Values[V].RemainingUses;
    RegValue[D] = V;
    if (hasSlot(D))
      storeToSlot(D, V);
    return;
  }
  auto RI = Remat.find(S);
  if (RI != Remat.end()) {
    Remat[D] = RI->second;
    if (hasSlot(D))
      storeOperand(D, Src);
    return;
  }
  if (hasSlot(D)) {
    // Uses of D in this block read D's own slot.
    storeOperand(D, Src);
    return;
  }
  if (Src.K == Operand::Slot) {
    // Alias the source slot unless it is written again later in this block
    // (by its register or by another register sharing the slot: the slot
    // allocator keeps a slot free only for the uses of an in-block copy,
    // not for a copy delivered to the successors at the block end).
    int FI = FuncInfo->getSpillSlot(Src.Reg);
    bool Redefined = false;
    bool After = false;
    for (const MachineInstr &O : *CurMBB) {
      if (&O == &MI) {
        After = true;
        continue;
      }
      if (!After)
        continue;
      for (const MachineOperand &MO : O.defs())
        if (MO.isReg() && MO.getReg().isVirtual() &&
            FuncInfo->getSpillSlot(MO.getReg()) == FI)
          Redefined = true;
    }
    if (!Redefined) {
      SlotAlias[D] = Src.Reg;
      return;
    }
  }
  // Load the value into flight.
  unsigned V = defineValue(D, MI);
  if (Values[V].RemainingUses == 0)
    return;
  expire(chainLength(Src) + 3);
  int Last;
  emitChain(Src, Last);
  int RefOp = chainRefOp(Src);
  produce(V, Last, RefOp, RefOp >= 0 ? 31 : 0);
}

/// CONST_*/ADDR_*: built at each use (and stored once if slot-resident);
/// one that travels in flight across edges is built here and delivered.
void ScryTemporalLowering::expandConstDef(MachineInstr &MI) {
  Register D = MI.getOperand(0).getReg();
  Remat[D] = &MI;
  RegValue.erase(D);
  SlotAlias.erase(D);
  if (hasSlot(D)) {
    storeOperand(D, operandFor(MI.getOperand(0)));
    return;
  }
  if (deliveredAtEnd(D, &MI)) {
    Operand Src = operandFor(MI.getOperand(0));
    Remat.erase(D);
    unsigned V = defineValue(D, MI);
    expire(chainLength(Src) + 3);
    int Last;
    emitChain(Src, Last);
    int RefOp = chainRefOp(Src);
    produce(V, Last, RefOp, RefOp >= 0 ? 31 : 0);
  }
}

void ScryTemporalLowering::expandPrologue(MachineBasicBlock &Entry) {
  unsigned NumArgs = FuncInfo->getNumArgOperands();
  unsigned NumReserves = llvm::popcount(ReserveSize);

  // Destination register of each argument delivered as an operand.
  SmallVector<Register, 4> ArgRegs(NumArgs, Register());
  SmallVector<const MachineInstr *, 4> ArgMIs(NumArgs, nullptr);
  for (MachineInstr &MI : Entry) {
    switch (MI.getOpcode()) {
    case Scry::ARG_8:
    case Scry::ARG_16:
    case Scry::ARG_32:
    case Scry::ARG_64: {
      unsigned Idx = MI.getOperand(1).getImm();
      if (Idx < NumArgs) {
        ArgRegs[Idx] = MI.getOperand(0).getReg();
        ArgMIs[Idx] = &MI;
      }
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

  // Values of the arguments.
  SmallVector<unsigned, 4> ArgVals;
  for (unsigned i = 0; i < NumArgs; ++i) {
    unsigned Uses = 0;
    if (ArgRegs[i].isValid())
      Uses = countUsesAfter(ArgRegs[i], *ArgMIs[i]) + extraUses(ArgRegs[i], ArgMIs[i]);
    ArgVals.push_back(newValue(Uses));
    if (ArgRegs[i].isValid())
      RegValue[ArgRegs[i]] = ArgVals.back();
  }

  if (NumArgs == 1) {
    // One argument: it lands on the first instruction.
    if (NumReserves > 0) {
      int H = emit(Scry::ECHOL, {0});
      emitReserves(ReserveSize, /*Private=*/true);
      Values[ArgVals[0]].Pending.push_back({H, 0, 1023});
      if (Values[ArgVals[0]].RemainingUses == 0) {
        int N = emit(Scry::NOP);
        bindOutput(Values[ArgVals[0]].Pending.back(), N);
        Values[ArgVals[0]].Pending.clear();
      }
    } else if (Values[ArgVals[0]].RemainingUses == 0) {
      emit(Scry::NOP);
    } else {
      int H = emit(Scry::ECHOL, {0});
      Values[ArgVals[0]].Pending.push_back({H, 0, 1023});
    }
    return;
  }

  if (NumReserves > 0)
    emit(Scry::ECHOL, {(int64_t)NumReserves});
  emitReserves(ReserveSize, /*Private=*/true);

  // Spread the argument list: echo =>a0, =>a1, => ; echo =>a2, =>a3.
  int S0 = emit(NumArgs >= 3 ? Scry::ECHOS : Scry::ECHO, {0, 0});
  Values[ArgVals[0]].Pending.push_back({S0, 0, 31});
  Values[ArgVals[1]].Pending.push_back({S0, 1, 31});
  if (NumArgs >= 3) {
    int S1 = emit(Scry::ECHO, {0, 0});
    Values[ArgVals[2]].Pending.push_back({S1, 0, 31});
    if (NumArgs >= 4)
      Values[ArgVals[3]].Pending.push_back({S1, 1, 31});
    else
      Cur->Items[S1].Imms[1] = 0; // no second operand; reference unused
  }
  // Unused arguments are discarded by nops right away.
  for (unsigned i = 0; i < NumArgs; ++i) {
    ValueState &VS = Values[ArgVals[i]];
    if (VS.RemainingUses == 0 && !VS.Pending.empty()) {
      int N = emit(Scry::NOP);
      bindOutput(VS.Pending.back(), N);
      VS.Pending.clear();
    }
  }
}

/// Liveness of the edge-flight registers on the final control flow graph,
/// and their landing positions: two registers arriving at the same block
/// get different positions (greedy colouring in register order).
void ScryTemporalLowering::planEdgeFlight() {
  EdgeLiveIn.clear();
  EdgePos.clear();
  SmallVector<Register, 32> Regs;
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    Register R = Register::index2VirtReg(i);
    if (FuncInfo->isEdgeFlight(R))
      Regs.push_back(R);
  }
  if (Regs.empty())
    return;
  DenseMap<Register, unsigned> IndexOf;
  for (unsigned i = 0, e = Regs.size(); i != e; ++i)
    IndexOf[Regs[i]] = i;

  // Upward-exposed uses and definitions per block, then live-in/live-out
  // by iteration.
  unsigned N = Regs.size();
  DenseMap<const MachineBasicBlock *, BitVector> Use, Def, LiveIn, LiveOut;
  for (MachineBasicBlock &MBB : *MF) {
    BitVector U(N), D(N);
    for (MachineInstr &MI : MBB) {
      if (MI.isDebugInstr())
        continue;
      for (MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || !MO.getReg().isVirtual())
          continue;
        auto It = IndexOf.find(MO.getReg());
        if (It == IndexOf.end())
          continue;
        if (MO.isUse() && !D[It->second])
          U[It->second] = true;
      }
      for (MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || !MO.isDef() || !MO.getReg().isVirtual())
          continue;
        auto It = IndexOf.find(MO.getReg());
        if (It != IndexOf.end())
          D[It->second] = true;
      }
    }
    Use[&MBB] = U;
    Def[&MBB] = D;
    LiveIn[&MBB] = BitVector(N);
    LiveOut[&MBB] = BitVector(N);
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (MachineBasicBlock &MBB : llvm::reverse(*MF)) {
      BitVector Out(N);
      for (MachineBasicBlock *S : MBB.successors())
        Out |= LiveIn[S];
      BitVector In = Out;
      In.reset(Def[&MBB]);
      In |= Use[&MBB];
      if (In != LiveIn[&MBB] || Out != LiveOut[&MBB]) {
        LiveIn[&MBB] = In;
        LiveOut[&MBB] = Out;
        Changed = true;
      }
    }
  }
  // Nothing arrives at the entry block (its arguments come from the caller).
  LiveIn[&MF->front()].reset();
  for (MachineBasicBlock &MBB : *MF)
    for (unsigned i : LiveIn[&MBB].set_bits())
      EdgeLiveIn[&MBB].push_back(Regs[i]);

  // Deliveries: every block delivers the live-ins of all its successors.
  DenseMap<const MachineBasicBlock *, BitVector> Deliver;
  for (MachineBasicBlock &P : *MF) {
    BitVector D(N);
    for (MachineBasicBlock *S : P.successors())
      D |= LiveIn[S];
    if (D.any())
      Deliver[&P] = std::move(D);
  }
  // Colour: registers delivered by the same block need different positions.
  // Two registers arriving at a block from different predecessors may share
  // one: the block needs at most one of them (a register it needs comes
  // from every predecessor, together with everything else they deliver),
  // and its pad at that position echoes the needed one or discards both.
  SmallVector<unsigned, 16> PositionOf(N, ~0u);
  for (unsigned i = 0; i != N; ++i) {
    BitVector Used;
    for (auto &KV : Deliver) {
      if (!KV.second[i])
        continue;
      for (unsigned j : KV.second.set_bits()) {
        if (j == i || PositionOf[j] == ~0u)
          continue;
        if (Used.size() <= PositionOf[j])
          Used.resize(PositionOf[j] + 1);
        Used[PositionOf[j]] = true;
      }
    }
    unsigned Pos = 0;
    while (Pos < Used.size() && Used[Pos])
      ++Pos;
    PositionOf[i] = Pos;
    EdgePos[Regs[i]] = {Pos, 0};
  }
}

/// The registers the current block delivers in flight to its successors:
/// the union of what is live into them.
void ScryTemporalLowering::computeDeliverSet(MachineBasicBlock &MBB) {
  DeliverSet.clear();
  for (MachineBasicBlock *S : MBB.successors())
    for (Register R : edgeLiveIns(S))
      DeliverSet.insert(R);
}

/// Receive the values arriving in flight at the block start.  Every
/// predecessor delivers the union of its successors' live-ins at fixed
/// positions (two registers per position); position p's pad is the p-th
/// instruction of the block: an echo re-aiming the values at their
/// consumers, a nop for a position nothing arrives at, and a nop sink for
/// values this block does not need.
void ScryTemporalLowering::emitLandingPads(MachineBasicBlock &MBB) {
  // The positions the block needs (every predecessor delivers those
  // registers), and the last position anything arrives at: whatever a
  // predecessor delivers for another successor is discarded here.
  std::map<unsigned, Register> Positions;
  for (Register R : edgeLiveIns(&MBB)) {
    unsigned Pos = EdgePos.lookup(R).first;
    assert(!Positions.count(Pos) && "position clash");
    Positions[Pos] = R;
  }
  int Last = -1;
  for (MachineBasicBlock *P : MBB.predecessors())
    for (MachineBasicBlock *S : P->successors())
      for (Register R : edgeLiveIns(S))
        Last = std::max(Last, (int)EdgePos.lookup(R).first);

  for (int Pos = 0; Pos <= Last; ++Pos) {
    auto It = Positions.find(Pos);
    if (It == Positions.end()) {
      int Pad = emit(Scry::NOP);
      Cur->Items[Pad].Comment = "landing pad " + std::to_string(Pos);
      continue;
    }
    Register R = It->second;
    unsigned Uses =
        countUsesFromStart(R) +
        (DeliverSet.count(R) && !definedLaterInBlock(R, nullptr) ? 1 : 0);
    int Pad;
    if (Uses == 0) {
      // Not needed here: discard it.
      Pad = emit(Scry::NOP);
    } else {
      Pad = emit(Scry::ECHOL, {0});
      unsigned V = newValue(Uses);
      RegValue[R] = V;
      Values[V].Pending.push_back({Pad, 0, 1023});
    }
    raw_string_ostream OS(Cur->Items[Pad].Comment);
    OS << "landing pad " << Pos << ": " << printReg(R);
  }
}

/// Aim the values the successors expect at their landing positions, which
/// follow the block's last instruction (the jump trigger).
void ScryTemporalLowering::deliverContract(MachineBasicBlock &MBB) {
  if (DeliverSet.empty())
    return;
  std::map<unsigned, Register> Positions;
  for (Register R : DeliverSet)
    Positions[EdgePos.lookup(R).first] = R;

  // Each position is its own consumer, so there is no ordering to fix:
  // take an output per value (copying or building it if needed), hop those
  // that could not reach their position even if every other one hopped
  // too, and bind once the end is known.
  struct Delivery {
    unsigned Pos;
    Output O;
  };
  // Everything pending must survive the copies, chains and hops emitted
  // below.
  unsigned Look = 2;
  for (auto &[Pos, R] : Positions)
    Look += chainLength(operandFor(MachineOperand::CreateReg(R, false))) + 3;
  expire(Look);
  SmallVector<Delivery, 8> Deliveries;
  // The worst-case end: every position copying (a dup) or building (a
  // chain and an echo) its value and hopping it.
  int Worst = curIndex();
  for (auto &[Pos, R] : Positions)
    Worst += chainLength(operandFor(MachineOperand::CreateReg(R, false))) + 2;
  for (auto &[Pos, R] : Positions) {
    Operand Op = operandFor(MachineOperand::CreateReg(R, false));
    Output O;
    if (Op.K == Operand::Flight) {
      ValueState &VS = Values[Op.V];
      if (VS.RemainingUses > VS.Pending.size())
        ensureOutputs(Op.V, VS.Pending.size() + 1);
      takeOutput(Op.V, -1, O);
      --Values[Op.V].RemainingUses;
    } else {
      int Last;
      emitChain(Op, Last);
      int H = emit(Scry::ECHOL, {0});
      ++FnHops;
      O = Output{H, 0, 1023};
    }
    // Hop right away if the output could not reach its position even in
    // the worst case (every remaining delivery hopping too).
    if (Cur->refValue(O.Item, Worst + 1 + (int)Pos, false) > O.Reach) {
      int H = emit(Scry::ECHOL, {0});
      bindOutput(O, H);
      ++FnHops;
      O = Output{H, 0, 1023};
    }
    Deliveries.push_back({Pos, O});
  }
  int End = curIndex() - 1;
  for (Delivery &D : Deliveries)
    bindOutput(D.O, End + 1 + (int)D.Pos);
  if (!Deliveries.empty() && Cur->Items[Deliveries.front().O.Item].Comment.empty())
    Cur->Items[Deliveries.front().O.Item].Comment = "deliver to successors";
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
  CurExpandMI = &MI;
  auto ClearCur = llvm::make_scope_exit([&] { CurExpandMI = nullptr; });

  switch (Opc) {
  // Already handled in the prologue / no code.
  case Scry::ARG_8:
  case Scry::ARG_16:
  case Scry::ARG_32:
  case Scry::ARG_64: {
    // The value was created by the prologue; store it if it has a slot.
    Register D = MI.getOperand(0).getReg();
    auto It = RegValue.find(D);
    if (It != RegValue.end() && hasSlot(D))
      storeToSlot(D, It->second);
    break;
  }
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
    expandCopy(MI);
    break;

  case Scry::CONST_8:
  case Scry::CONST_16:
  case Scry::CONST_32:
  case Scry::CONST_64:
  case Scry::ADDR_SYM:
  case Scry::ADDR_FI:
  case Scry::ADDR_OUT:
    if (Opc == Scry::ADDR_FI && !MI.getOperand(1).isImm())
      report_fatal_error("Scry: unresolved frame index");
    expandConstDef(MI);
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
    Operand C = operandFor(MI.getOperand(1));
    Operand T = operandFor(MI.getOperand(2));
    Operand F = operandFor(MI.getOperand(3));
    unsigned V = defineValue(D, MI);
    int P = consume(Scry::PICK, {C, F, T}, {0});
    produce(V, P, 0, 31);
    defineResult(MI, V);
    break;
  }

  case Scry::ZEXT_8_16: case Scry::ZEXT_8_32: case Scry::ZEXT_8_64:
  case Scry::ZEXT_16_32: case Scry::ZEXT_16_64: case Scry::ZEXT_32_64:
    if (tryElideZext(MI))
      break;
    [[fallthrough]];
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
    Operand A = operandFor(MI.getOperand(1));
    A.RelBase = A.K == Operand::SymAddr;
    unsigned V = defineValue(D, MI);
    int L = consume(Scry::LD, {A}, {(int64_t)utype(D), 0});
    produce(V, L, 1, 31);
    defineResult(MI, V);
    break;
  }
  case Scry::ST_8: case Scry::ST_16: case Scry::ST_32: case Scry::ST_64: {
    Operand Val = operandFor(MI.getOperand(0));
    Operand A = operandFor(MI.getOperand(1));
    A.RelBase = A.K == Operand::SymAddr;
    consume(Scry::ST, {Val, A});
    break;
  }
  // Indexed forms: [base, idx], the index scaled by the access size.
  case Scry::LDX_8: case Scry::LDX_16: case Scry::LDX_32: case Scry::LDX_64: {
    Register D = MI.getOperand(0).getReg();
    Operand A = operandFor(MI.getOperand(1));
    Operand I = operandFor(MI.getOperand(2));
    A.RelBase = A.K == Operand::SymAddr;
    unsigned V = defineValue(D, MI);
    int L = consume(Scry::LD, {A, I}, {(int64_t)utype(D), 0});
    produce(V, L, 1, 31);
    defineResult(MI, V);
    break;
  }
  case Scry::STX_8: case Scry::STX_16: case Scry::STX_32: case Scry::STX_64: {
    Operand Val = operandFor(MI.getOperand(0));
    Operand A = operandFor(MI.getOperand(1));
    Operand I = operandFor(MI.getOperand(2));
    A.RelBase = A.K == Operand::SymAddr;
    consume(Scry::ST, {Val, A, I});
    break;
  }

  case Scry::BR:
  case Scry::BRCOND:
    return; // issued by emitJump at their boundary

  case Scry::RETURN:
    expandReturn(MI);
    break;

  case Scry::CALL_V: case Scry::CALL_8: case Scry::CALL_16: case Scry::CALL_32: case Scry::CALL_64:
  case Scry::CALLI_V: case Scry::CALLI_8: case Scry::CALLI_16: case Scry::CALLI_32: case Scry::CALLI_64:
    expandCall(MI);
    break;

  case Scry::OUT_RSRV:
    expire(llvm::popcount((unsigned)MI.getOperand(0).getImm()) + 2);
    emitReserves((unsigned)MI.getOperand(0).getImm(), /*Private=*/false);
    break;
  case Scry::OUT_FREE: {
    unsigned Bytes = (unsigned)MI.getOperand(0).getImm();
    expire(llvm::popcount(Bytes) + 2);
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
    expire(3);
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

  if (curIndex() > (int)Before && Cur->Items[Before].Comment.empty())
    Cur->Items[Before].Comment = describe(MI);
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

bool ScryTemporalLowering::expandFunction() {
  Blocks.clear();
  FoldedNegations.clear();
  JumpOnlyCompares.clear();
  JumpOnlyInfo.clear();
  FnNearJumps = FnFarJumps = FnNegations = 0;
  FnHops = FnDups = FnSlotLoads = FnSlotStores = FnElidedCasts = 0;
  Blocks.reserve(MF->size());
  for (MachineBasicBlock &MBB : *MF) {
    Blocks.push_back(BlockSeq{&MBB, {}, {}, 0});
    Cur = &Blocks.back();
    CurMBB = &MBB;
    Values.clear();
    RegValue.clear();
    SlotAlias.clear();
    Remat.clear();
    NarrowRegs.clear();

    computeDeliverSet(MBB);
    if (&MBB == &MF->front())
      expandPrologue(MBB);
    else
      emitLandingPads(MBB);
    SmallVector<PlannedJump, 2> Jumps;
    planJumps(MBB, Jumps);
    planNegationFolds(MBB, Jumps);
    auto EmitJumpsAt = [&](unsigned Boundary) {
      for (const PlannedJump &J : Jumps)
        if (J.Boundary == Boundary)
          emitJump(MBB, J);
    };
    EmitJumpsAt(0);
    for (MachineInstr &MI : MBB) {
      expandInstr(MI, MBB);
      if (!MI.isTerminator())
        EmitJumpsAt(MIIndex.lookup(&MI) + 1);
    }
    // A jump trigger must not share its address with a call trigger (the
    // later issued call would replace the jump in the trigger map), so a
    // block ending in a call gets a nop for its jumps to trigger at; it
    // comes before the deliveries, which aim past the block's last item.
    if (!Cur->Items.empty() && Cur->Items.back().Opc == Scry::CALL &&
        llvm::any_of(Cur->Items, [](const Item &I) { return I.JumpTarget; }))
      emit(Scry::NOP);
    deliverContract(MBB);
    // Every block occupies at least one instruction so that a forward jump
    // never targets the instruction right after its own trigger.
    if (Cur->Items.empty())
      emit(Scry::NOP);
    // Everything in flight must have been consumed within the block.
    for (const ValueState &VS : Values) {
      assert(VS.Pending.empty() && "value left in flight at block end");
      assert(VS.RemainingUses == 0 && "value with unserved uses");
      (void)VS;
    }
    // All jumps trigger after the block's last instruction.
    int End = curIndex() - 1;
    for (Item &I : Cur->Items)
      if (I.JumpTarget)
        I.TrigTarget = End;
  }

  // Resolve local references and triggers, and lay out the blocks.
  bool Changed = false;
  unsigned Abs = 0;
  for (BlockSeq &B : Blocks) {
    B.Start = Abs;
    for (unsigned i = 0, e = B.Items.size(); i != e; ++i) {
      Item &I = B.Items[i];
      if (I.RefOp >= 0) {
        int Ref = B.refValue(i, I.RefTarget, I.RefArg);
        assert(Ref >= 0 && "backward reference");
        if (Ref > (I.Opc == Scry::ECHOL ? 1023 : 31))
          report_fatal_error("Scry: forward reference out of range");
        I.Imms[I.RefOp] = Ref;
      }
      if (I.Ref2Op >= 0) {
        int Ref = B.refValue(i, I.Ref2Target, I.Ref2Arg);
        assert(Ref >= 0 && Ref <= 31 && "bad second reference");
        I.Imms[I.Ref2Op] = Ref;
      }
      if (I.TrigTarget >= 0) {
        int Loc = I.TrigTarget - (int)i;
        assert(Loc >= 0 && "trigger before the jump");
        if (!I.FarJump) {
          if (Loc > 63) {
            // Out of range for the near form: widen this jump.
            JumpForms[I.JumpKey] = JF_Far8;
            Changed = true;
          } else {
            I.Imms[1] = Loc;
          }
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
  if (Changed)
    return false;

  DenseMap<MachineBasicBlock *, unsigned> Starts;
  for (BlockSeq &B : Blocks)
    Starts[B.MBB] = B.Start;

  // Relative targets: a backward jump targets jmp + t (t <= 0) and is taken
  // on a non-zero condition; a forward jump targets trigger + t + 1 (t >= 1)
  // and is taken on zero.  A jump whose form cannot express its target moves
  // to the next form and the function is expanded again.
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
  NumHops += FnHops;
  NumDups += FnDups;
  NumElidedCasts += FnElidedCasts;
  ElidedBefore += FnElidedCasts;
  NumSlotLoads += FnSlotLoads;
  NumSlotStores += FnSlotStores;
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
        unsigned Flags = (I.PCRel ? Scry::MO_PCREL_B0 : Scry::MO_B0) + I.SymByte;
        if (I.GV)
          MIB.addGlobalAddress(I.GV, I.SymOffset, Flags);
        else if (I.ExtSym) {
          MachineOperand MO = MachineOperand::CreateES(I.ExtSym, Flags);
          MO.setOffset(I.SymOffset);
          MIB.add(MO);
        } else if (I.BA)
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
  MIIndex.clear();
  unsigned Idx = 0;
  for (MachineBasicBlock &MBB : *MF) {
    LayoutIndex[&MBB] = Idx++;
    unsigned N = 0;
    for (MachineInstr &MI : MBB)
      MIIndex[&MI] = N++;
  }
  planEdgeFlight();

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
