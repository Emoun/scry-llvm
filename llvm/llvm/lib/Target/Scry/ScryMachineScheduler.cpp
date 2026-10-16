//===-- ScryMachineScheduler.cpp - Scry pre-emission scheduler ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryMachineScheduler.h"
#include "Scry.h"
#include "ScryInstrInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "scry-sched"

static cl::opt<unsigned> InFlightTarget(
    "scry-inflight-target", cl::Hidden, cl::init(32),
    cl::desc("Soft limit on the number of values in flight within a block; "
             "above it consumers are preferred over producers"));

static cl::opt<bool> ConsumersLast(
    "scry-sched-consumers-last", cl::Hidden, cl::init(true),
    cl::desc("Schedule the consumers of a load's result after any other "
             "ready instruction"));

static cl::opt<bool> LoadsEarly(
    "scry-sched-loads-early", cl::Hidden, cl::init(true),
    cl::desc("Schedule loads, and the instructions computing their "
             "addresses, before the rest of the block"));

namespace {

/// Instructions the emitter rebuilds in front of their consumer: their
/// position is irrelevant and they produce nothing in flight.
static bool isRematerialised(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case Scry::CONST_8:
  case Scry::CONST_16:
  case Scry::CONST_32:
  case Scry::CONST_64:
  case Scry::ADDR_SYM:
  case Scry::ADDR_FI:
  case Scry::ADDR_OUT:
    return true;
  default:
    return false;
  }
}

class ScrySchedStrategy final : public MachineSchedStrategy {
  ScheduleDAGMI *DAG = nullptr;
  SmallVector<SUnit *, 32> Ready;
  DenseSet<const SUnit *> CondSlice;
  DenseSet<const SUnit *> LoadSlice;
  DenseMap<const SUnit *, unsigned> SchedPos;
  unsigned Clock = 0;
  unsigned InFlight = 0;

  static bool isDataPred(const SDep &D) {
    return D.getKind() == SDep::Data;
  }

  /// Position of the oldest in-flight input of SU (Clock when none).
  unsigned oldestInput(const SUnit *SU) const {
    unsigned Oldest = Clock;
    for (const SDep &D : SU->Preds) {
      if (!isDataPred(D) || isRematerialised(*D.getSUnit()->getInstr()))
        continue;
      auto It = SchedPos.find(D.getSUnit());
      if (It != SchedPos.end())
        Oldest = std::min(Oldest, It->second);
    }
    return Oldest;
  }

  /// Number of in-flight values whose last consumer is SU.
  unsigned valuesFreed(const SUnit *SU) const {
    unsigned N = 0;
    for (const SDep &D : SU->Preds) {
      const SUnit *P = D.getSUnit();
      if (!isDataPred(D) || isRematerialised(*P->getInstr()))
        continue;
      bool Last = true;
      for (const SDep &S : P->Succs)
        if (isDataPred(S) && S.getSUnit() != SU && !S.getSUnit()->isScheduled)
          Last = false;
      N += Last;
    }
    return N;
  }

  bool producesValue(const SUnit *SU) const {
    if (isRematerialised(*SU->getInstr()))
      return false;
    for (const SDep &S : SU->Succs)
      if (isDataPred(S))
        return true;
    return false;
  }

  /// Operand index at which the result of P is used by S, or -1.
  static int operandIndexIn(const SUnit *P, const SUnit *S) {
    const MachineInstr &MP = *P->getInstr(), &MS = *S->getInstr();
    if (MP.getNumExplicitDefs() != 1 || !MP.getOperand(0).isReg())
      return -1;
    Register R = MP.getOperand(0).getReg();
    for (unsigned i = 0, e = MS.getNumOperands(); i != e; ++i) {
      const MachineOperand &MO = MS.getOperand(i);
      if (MO.isReg() && MO.isUse() && MO.getReg() == R)
        return (int)i;
    }
    return -1;
  }

  /// When A and B feed the same consumer, the one it uses first should be
  /// emitted first: the consumer takes its operands in the order their
  /// producers executed, and anything else costs an echo to reorder.
  /// Returns -1 (A first), 1 (B first) or 0 (no common consumer / same).
  static int operandOrder(const SUnit *A, const SUnit *B) {
    for (const SDep &SA : A->Succs) {
      if (SA.getKind() != SDep::Data)
        continue;
      const SUnit *S = SA.getSUnit();
      if (S->isScheduled)
        continue;
      bool Common = false;
      for (const SDep &SB : B->Succs)
        if (SB.getKind() == SDep::Data && SB.getSUnit() == S)
          Common = true;
      if (!Common)
        continue;
      int IA = operandIndexIn(A, S), IB = operandIndexIn(B, S);
      if (IA >= 0 && IB >= 0 && IA != IB)
        return IA < IB ? -1 : 1;
    }
    return 0;
  }

  /// Does SU consume the result of a memory load already scheduled?  Such
  /// consumers wait while anything else is ready, so that loads deliver as
  /// far ahead of their use as the block allows.
  bool consumesLoad(const SUnit *SU) const {
    for (const SDep &D : SU->Preds) {
      const SUnit *P = D.getSUnit();
      if (isDataPred(D) && P->isScheduled && P->getInstr()->mayLoad() &&
          !isRematerialised(*P->getInstr()))
        return true;
    }
    return false;
  }

  /// Is A to be scheduled before B?
  bool better(const SUnit *A, const SUnit *B) const {
    const MachineInstr &MA = *A->getInstr(), &MB = *B->getInstr();
    bool CA = CondSlice.count(A), CB = CondSlice.count(B);
    if (CA != CB)
      return CA;
    bool LA = LoadSlice.count(A), LB = LoadSlice.count(B);
    if (LA != LB)
      return LA;
    if (LA && MA.mayLoad() && MB.mayLoad() && A->getHeight() != B->getHeight())
      return A->getHeight() > B->getHeight(); // the load with the most work behind it
    bool RA = isRematerialised(MA), RB = isRematerialised(MB);
    if (RA != RB)
      return RB; // constants last: their position does not matter
    if (InFlight > InFlightTarget) {
      int FA = (int)valuesFreed(A) - (int)producesValue(A);
      int FB = (int)valuesFreed(B) - (int)producesValue(B);
      if (FA != FB)
        return FA > FB;
    }
    if (ConsumersLast) {
      bool DA = consumesLoad(A), DB = consumesLoad(B);
      if (DA != DB)
        return DB; // the one that does not use a load's result
    }
    if (int Order = operandOrder(A, B))
      return Order < 0;
    unsigned OA = oldestInput(A), OB = oldestInput(B);
    if (OA != OB)
      return OA > OB; // the one whose inputs are freshest
    if (A->getHeight() != B->getHeight())
      return A->getHeight() > B->getHeight();
    return A->NodeNum < B->NodeNum;
  }

public:
  MachineSchedPolicy getPolicy() const override {
    MachineSchedPolicy P;
    P.OnlyTopDown = true;
    P.ShouldTrackPressure = false;
    return P;
  }
  bool shouldTrackPressure() const override { return false; }

  /// Add Root and everything it depends on through data edges to Slice.
  static void addSlice(DenseSet<const SUnit *> &Slice, const SUnit *Root) {
    SmallVector<const SUnit *, 16> Work{Root};
    while (!Work.empty()) {
      const SUnit *SU = Work.pop_back_val();
      if (!Slice.insert(SU).second)
        continue;
      for (const SDep &D : SU->Preds)
        if (isDataPred(D))
          Work.push_back(D.getSUnit());
    }
  }

  void initialize(ScheduleDAGMI *dag) override {
    DAG = dag;
    Ready.clear();
    CondSlice.clear();
    LoadSlice.clear();
    SchedPos.clear();
    Clock = 0;
    InFlight = 0;
    if (DAG->SUnits.empty())
      return;

    // The instructions feeding the block's branch condition.
    MachineBasicBlock *MBB = DAG->SUnits.front().getInstr()->getParent();
    Register Cond;
    for (MachineInstr &MI : MBB->terminators())
      if (MI.getOpcode() == Scry::BRCOND)
        Cond = MI.getOperand(0).getReg();
    if (Cond.isValid()) {
      const MachineRegisterInfo &MRI = DAG->MRI;
      MachineInstr *Def = MRI.hasOneDef(Cond) ? MRI.getVRegDef(Cond) : nullptr;
      if (Def && Def->getParent() == MBB)
        if (SUnit *Root = DAG->getSUnit(Def))
          addSlice(CondSlice, Root);
    }


    // The loads and the instructions computing their addresses.
    if (LoadsEarly)
      for (const SUnit &SU : DAG->SUnits)
        if (SU.getInstr()->mayLoad())
          addSlice(LoadSlice, &SU);
  }

  void releaseTopNode(SUnit *SU) override { Ready.push_back(SU); }
  void releaseBottomNode(SUnit *SU) override {}

  SUnit *pickNode(bool &IsTopNode) override {
    IsTopNode = true;
    if (Ready.empty())
      return nullptr;
    unsigned Best = 0;
    for (unsigned i = 1, e = Ready.size(); i != e; ++i)
      if (better(Ready[i], Ready[Best]))
        Best = i;
    SUnit *SU = Ready[Best];
    Ready[Best] = Ready.back();
    Ready.pop_back();
    return SU;
  }

  void schedNode(SUnit *SU, bool IsTopNode) override {
    InFlight -= std::min(InFlight, valuesFreed(SU));
    SchedPos[SU] = Clock++;
    if (producesValue(SU))
      ++InFlight;
  }
};

} // end anonymous namespace

ScheduleDAGInstrs *llvm::createScryMachineScheduler(MachineSchedContext *C) {
  return new ScheduleDAGMILive(C, std::make_unique<ScrySchedStrategy>());
}
