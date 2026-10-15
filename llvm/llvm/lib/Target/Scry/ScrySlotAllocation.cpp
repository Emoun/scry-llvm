//===-- ScrySlotAllocation.cpp - Assign frame slots to virtual registers --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scry has no registers, so every virtual register produced by instruction
// selection is given a slot in the function's private stack frame.  This pass
// runs before prologue/epilogue insertion so that the generic frame layout
// code assigns the slots their final offsets; ScryTemporalLowering then
// rewrites all register traffic into stack loads and stores.
//
// Registers with a single definition whose uses all follow it in the same
// block get no slot: ScryTemporalLowering keeps them in flight between their
// producer and their consumers.  The remaining ("cross-block") registers
// share slots when their live intervals do not overlap (greedy interval
// colouring per slot size), which keeps frames small: the scryer simulator
// only has a few kilobytes of stack.  Sharing is safe because a slot is
// written right after its definition and read at each use.
//
//===----------------------------------------------------------------------===//

#include "Scry.h"
#include "ScryMachineFunctionInfo.h"
#include "ScrySubtarget.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "scry-slot-alloc"

static cl::opt<bool> ShareSlots(
    "scry-share-slots", cl::Hidden, cl::init(true),
    cl::desc("Let virtual registers with disjoint live ranges share a slot"));

static cl::opt<bool> InFlightValues(
    "scry-in-flight", cl::Hidden, cl::init(true),
    cl::desc("Keep single-block values in flight instead of in frame slots"));

namespace {

class ScrySlotAllocation final : public MachineFunctionPass {
public:
  static char ID;
  ScrySlotAllocation() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Scry virtual register slot allocation";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    if (ShareSlots)
      AU.addRequired<LiveIntervalsWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

struct Slot {
  int FI;
  SmallVector<const LiveInterval *, 4> Occupants;
};

/// Moves the ARG_* pseudos to the start of the entry block.  The prologue
/// stores every operand argument into its slot immediately on entry, so the
/// register is live from there regardless of where the scheduler left the
/// pseudo; the live intervals used for slot sharing must reflect that.
class ScryArgHoisting final : public MachineFunctionPass {
public:
  static char ID;
  ScryArgHoisting() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "Scry argument hoisting"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (MF.empty())
      return false;
    MachineBasicBlock &Entry = MF.front();
    MachineBasicBlock::iterator InsertPt = Entry.begin();
    bool Changed = false;
    for (MachineInstr &MI : llvm::make_early_inc_range(Entry)) {
      switch (MI.getOpcode()) {
      case Scry::ARG_8:
      case Scry::ARG_16:
      case Scry::ARG_32:
      case Scry::ARG_64:
        if (MI.getIterator() != InsertPt) {
          Entry.splice(InsertPt, &Entry, MI.getIterator());
          Changed = true;
        } else {
          ++InsertPt;
        }
        break;
      default:
        break;
      }
    }
    return Changed;
  }
};

} // end anonymous namespace

char ScryArgHoisting::ID = 0;

INITIALIZE_PASS(ScryArgHoisting, "scry-arg-hoist", "Scry argument hoisting",
                false, false)

FunctionPass *llvm::createScryArgHoistingPass() {
  return new ScryArgHoisting();
}

char ScrySlotAllocation::ID = 0;

INITIALIZE_PASS_BEGIN(ScrySlotAllocation, DEBUG_TYPE,
                      "Scry virtual register slot allocation", false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(ScrySlotAllocation, DEBUG_TYPE,
                    "Scry virtual register slot allocation", false, false)

FunctionPass *llvm::createScrySlotAllocationPass() {
  return new ScrySlotAllocation();
}

bool ScrySlotAllocation::runOnMachineFunction(MachineFunction &MF) {
  MachineRegisterInfo &MRI = MF.getRegInfo();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  auto *FuncInfo = MF.getInfo<ScryFunctionInfo>();
  LiveIntervals *LIS =
      ShareSlots ? &getAnalysis<LiveIntervalsWrapperPass>().getLIS() : nullptr;

  // Position of every instruction within its block, to tell whether a use
  // follows the definition.
  DenseMap<const MachineInstr *, unsigned> Index;
  for (MachineBasicBlock &MBB : MF) {
    unsigned N = 0;
    for (MachineInstr &MI : MBB)
      Index[&MI] = N++;
  }
  // One-byte constants are cheaper to rebuild than to load.
  auto IsByteConstant = [&](const MachineInstr *Def) {
    switch (Def->getOpcode()) {
    case Scry::CONST_8:
      return true;
    case Scry::CONST_16:
      return ((uint64_t)Def->getOperand(1).getImm() & 0xff00) == 0;
    case Scry::CONST_32:
      return ((uint64_t)Def->getOperand(1).getImm() & 0xffffff00) == 0;
    case Scry::CONST_64:
      return ((uint64_t)Def->getOperand(1).getImm() & ~0xffull) == 0;
    default:
      return false;
    }
  };
  auto StaysInFlight = [&](Register Reg) {
    if (!InFlightValues || !MRI.hasOneDef(Reg))
      return false;
    const MachineInstr *Def = MRI.getVRegDef(Reg);
    if (IsByteConstant(Def))
      return true;
    for (const MachineInstr &Use : MRI.use_nodbg_instructions(Reg))
      if (Use.getParent() != Def->getParent() || Index[&Use] <= Index[Def])
        return false;
    return true;
  };

  auto IntervalOf = [&](Register Reg) -> const LiveInterval * {
    if (!LIS)
      return nullptr;
    if (LIS->hasInterval(Reg))
      return &LIS->getInterval(Reg);
    return &LIS->createAndComputeVirtRegInterval(Reg);
  };

  // Collect the registers needing a slot with their sizes.  A register kept
  // in flight that is a copy of a slot register reads that slot at its uses
  // (ScryTemporalLowering aliases the copy), so the slot stays busy for the
  // copy's live range as well.
  struct Entry {
    Register Reg;
    unsigned Size;
    SmallVector<const LiveInterval *, 2> LIs;
  };
  SmallVector<Entry, 64> Entries;
  DenseMap<Register, unsigned> EntryOf;
  SmallVector<Register, 32> InFlightCopies;
  for (unsigned i = 0, e = MRI.getNumVirtRegs(); i != e; ++i) {
    Register Reg = Register::index2VirtReg(i);
    if (MRI.reg_nodbg_empty(Reg))
      continue;
    if (StaysInFlight(Reg)) {
      if (MRI.getVRegDef(Reg)->isCopy())
        InFlightCopies.push_back(Reg);
      continue;
    }
    unsigned Size = TRI.getRegSizeInBits(*MRI.getRegClass(Reg)) / 8;
    const LiveInterval *LI = IntervalOf(Reg);
    EntryOf[Reg] = Entries.size();
    Entries.push_back({Reg, Size, {}});
    if (LI && !LI->empty())
      Entries.back().LIs.push_back(LI);
  }
  // Follow copy chains to the slot register they read.
  llvm::sort(InFlightCopies, [&](Register A, Register B) {
    const MachineInstr *DA = MRI.getVRegDef(A), *DB = MRI.getVRegDef(B);
    if (DA->getParent() != DB->getParent())
      return DA->getParent()->getNumber() < DB->getParent()->getNumber();
    return Index[DA] < Index[DB];
  });
  DenseMap<Register, Register> AliasRoot;
  for (Register Reg : InFlightCopies) {
    Register Src = MRI.getVRegDef(Reg)->getOperand(1).getReg();
    auto RI = AliasRoot.find(Src);
    if (RI != AliasRoot.end())
      Src = RI->second;
    auto EI = EntryOf.find(Src);
    if (EI == EntryOf.end())
      continue;
    AliasRoot[Reg] = Src;
    const LiveInterval *LI = IntervalOf(Reg);
    if (LI && !LI->empty())
      Entries[EI->second].LIs.push_back(LI);
  }

  // Allocate the widest slots first so that as many as possible are
  // reachable with the short (5-bit index) stack load/store forms; within a
  // size, order by live range start for the greedy colouring.
  llvm::sort(Entries, [](const Entry &A, const Entry &B) {
    if (A.Size != B.Size)
      return A.Size > B.Size;
    if (!A.LIs.empty() && !B.LIs.empty())
      return *A.LIs.front() < *B.LIs.front();
    return A.Reg < B.Reg;
  });

  SmallVector<Slot, 64> Slots;
  unsigned CurSize = 0;
  size_t SizeStart = 0;
  for (const Entry &E : Entries) {
    if (E.Size != CurSize) {
      CurSize = E.Size;
      SizeStart = Slots.size();
    }
    int FI = -1;
    if (LIS && !E.LIs.empty()) {
      for (size_t s = SizeStart; s != Slots.size(); ++s) {
        bool Free = true;
        for (const LiveInterval *Other : Slots[s].Occupants)
          for (const LiveInterval *Mine : E.LIs)
            if (Other->overlaps(*Mine)) {
              Free = false;
              break;
            }
        if (Free) {
          FI = Slots[s].FI;
          Slots[s].Occupants.append(E.LIs.begin(), E.LIs.end());
          break;
        }
      }
    }
    if (FI < 0) {
      FI = MFI.CreateSpillStackObject(E.Size, Align(E.Size));
      Slots.push_back({FI, {}});
      Slots.back().Occupants.append(E.LIs.begin(), E.LIs.end());
    }
    FuncInfo->setSpillSlot(E.Reg, FI);
  }
  return true;
}
