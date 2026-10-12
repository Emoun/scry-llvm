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
// Registers whose live intervals do not overlap share a slot (greedy interval
// colouring per slot size), which keeps frames small: the scryer simulator
// only has a few kilobytes of stack.  Sharing is safe because every expansion
// in ScryTemporalLowering loads all of its inputs before storing its outputs.
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

  // Collect the live registers with their sizes.
  struct Entry {
    Register Reg;
    unsigned Size;
    const LiveInterval *LI;
  };
  SmallVector<Entry, 64> Entries;
  for (unsigned i = 0, e = MRI.getNumVirtRegs(); i != e; ++i) {
    Register Reg = Register::index2VirtReg(i);
    if (MRI.reg_nodbg_empty(Reg))
      continue;
    unsigned Size = TRI.getRegSizeInBits(*MRI.getRegClass(Reg)) / 8;
    const LiveInterval *LI = nullptr;
    if (LIS && LIS->hasInterval(Reg))
      LI = &LIS->getInterval(Reg);
    else if (LIS)
      LI = &LIS->createAndComputeVirtRegInterval(Reg);
    Entries.push_back({Reg, Size, LI});
  }

  // Allocate the widest slots first so that as many as possible are
  // reachable with the short (5-bit index) stack load/store forms; within a
  // size, order by live range start for the greedy colouring.
  llvm::sort(Entries, [](const Entry &A, const Entry &B) {
    if (A.Size != B.Size)
      return A.Size > B.Size;
    if (A.LI && B.LI && !A.LI->empty() && !B.LI->empty())
      return *A.LI < *B.LI;
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
    if (LIS && E.LI && !E.LI->empty()) {
      for (size_t s = SizeStart; s != Slots.size(); ++s) {
        bool Free = true;
        for (const LiveInterval *Other : Slots[s].Occupants)
          if (Other->overlaps(*E.LI)) {
            Free = false;
            break;
          }
        if (Free) {
          FI = Slots[s].FI;
          Slots[s].Occupants.push_back(E.LI);
          break;
        }
      }
    }
    if (FI < 0) {
      FI = MFI.CreateSpillStackObject(E.Size, Align(E.Size));
      Slots.push_back({FI, {}});
      if (E.LI && !E.LI->empty())
        Slots.back().Occupants.push_back(E.LI);
    }
    FuncInfo->setSpillSlot(E.Reg, FI);
  }
  return true;
}
