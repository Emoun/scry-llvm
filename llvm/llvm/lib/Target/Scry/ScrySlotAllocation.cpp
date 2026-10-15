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
// producer and their consumers.  Values live across blocks may travel in
// flight over the edges as well (see runOnMachineFunction).  The remaining
// ("cross-block") registers share slots when their live intervals do not overlap (greedy interval
// colouring per slot size), which keeps frames small: the scryer simulator
// only has a few kilobytes of stack.  Sharing is safe because a slot is
// written right after its definition and read at each use.
//
//===----------------------------------------------------------------------===//

#include "Scry.h"
#include "ScryMachineFunctionInfo.h"
#include "ScrySubtarget.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
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

static cl::opt<bool> EdgeFlight(
    "scry-edge-flight", cl::Hidden, cl::init(true),
    cl::desc("Carry values live across blocks in flight over the edges"));

static cl::opt<unsigned> FlightBlocks(
    "scry-flight-blocks", cl::Hidden, cl::init(8),
    cl::desc("Carry a value in flight only if it is live into at most this "
             "many blocks"));

static cl::opt<unsigned> FlightCostPercent(
    "scry-flight-cost", cl::Hidden, cl::init(70),
    cl::desc("Weight (percent) of a landing-pad instruction against a slot "
             "load or store when choosing between flight and slot"));

static cl::opt<unsigned> MaxEdgeValues(
    "scry-max-edge-values", cl::Hidden, cl::init(32),
    cl::desc("Maximum number of values arriving in flight at one block"));

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
    if (ShareSlots || EdgeFlight)
      AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addRequired<MachineLoopInfoWrapperPass>();
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
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
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
  LiveIntervals *LIS = ShareSlots || EdgeFlight
                           ? &getAnalysis<LiveIntervalsWrapperPass>().getLIS()
                           : nullptr;
  MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  // Instructions in loops execute more often: weight them by 8 per level.
  auto Weight = [&](const MachineBasicBlock *MBB) -> unsigned {
    unsigned Depth = std::min(MLI.getLoopDepth(MBB), 4u);
    return 1u << (3 * Depth);
  };
  // Instructions the lowering can rebuild in front of a use, with the
  // number of instructions that costs; 0 for everything else.
  auto RematLen = [&](const MachineInstr *Def) -> unsigned {
    switch (Def->getOpcode()) {
    case Scry::CONST_8:
      return 1;
    case Scry::CONST_16:
    case Scry::CONST_32:
    case Scry::CONST_64: {
      unsigned Scale = Def->getOpcode() == Scry::CONST_16 ? 2
                       : Def->getOpcode() == Scry::CONST_32 ? 4 : 8;
      uint64_t Imm = Def->getOperand(1).getImm();
      return std::min(scryConstBytesLen(Imm, Scale, false),
                      scryConstBytesLen(Imm, Scale, true));
    }
    case Scry::ADDR_SYM: {
      // Three instructions when every use is the address of a load, store
      // or call (PC-relative), four otherwise.
      Register R = Def->getOperand(0).getReg();
      for (const MachineOperand &MO : MRI.use_nodbg_operands(R)) {
        const MachineInstr &Use = *MO.getParent();
        unsigned Idx = MO.getOperandNo();
        switch (Use.getOpcode()) {
        case Scry::LD_8: case Scry::LD_16: case Scry::LD_32: case Scry::LD_64:
        case Scry::LDX_8: case Scry::LDX_16: case Scry::LDX_32: case Scry::LDX_64:
        case Scry::ST_8: case Scry::ST_16: case Scry::ST_32: case Scry::ST_64:
        case Scry::STX_8: case Scry::STX_16: case Scry::STX_32: case Scry::STX_64:
          if (Idx != 1)
            return 4;
          break;
        case Scry::CALLI_V:
          if (Idx != 0)
            return 4;
          break;
        case Scry::CALLI_8: case Scry::CALLI_16: case Scry::CALLI_32: case Scry::CALLI_64:
          if (Idx != 1)
            return 4;
          break;
        default:
          return 4;
        }
      }
      return 3;
    }
    case Scry::ADDR_FI:
      return 1;
    case Scry::ADDR_OUT:
      return 2;
    default:
      return 0;
    }
  };
  // Dynamic weight of every definition and use of a register.
  auto Hotness = [&](Register Reg) {
    unsigned H = 0;
    for (const MachineInstr &MI : MRI.reg_nodbg_instructions(Reg))
      H += Weight(MI.getParent());
    return H;
  };

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

  // Values live across blocks may travel in flight instead of through a
  // slot: every predecessor aims them at fixed positions of the successor,
  // whose landing pad (one echo per position) carries them on.  A register
  // qualifies when it is live into at most FlightBlocks blocks; registers
  // that always arrive together are paired in one position, and positions
  // are coloured so that registers arriving at the same block differ.
  // A constant or address with uses outside its block (typically hoisted
  // out of a loop) may instead be rebuilt at each use; the cheapest of
  // rebuilding, slot and flight wins.
  DenseSet<Register> RematRegs;
  DenseMap<Register, SmallVector<MachineBasicBlock *, 4>> LiveInBlocks;
  // Slots beyond the reach of the short forms (31 scaled indices) cost an
  // extra frame-address instruction per access; estimate from how much the
  // candidates would occupy whether that is to be expected.
  unsigned CandidateBytes = 0;
  for (unsigned i = 0, e = MRI.getNumVirtRegs(); i != e; ++i) {
    Register Reg = Register::index2VirtReg(i);
    if (!MRI.reg_nodbg_empty(Reg) && !StaysInFlight(Reg))
      CandidateBytes += TRI.getRegSizeInBits(*MRI.getRegClass(Reg)) / 8;
  }
  unsigned SlotAccess10 = CandidateBytes > 512 ? 20 : CandidateBytes > 256 ? 15 : 10;
  if (LIS) {
    for (unsigned i = 0, e = MRI.getNumVirtRegs(); i != e; ++i) {
      Register Reg = Register::index2VirtReg(i);
      if (MRI.reg_nodbg_empty(Reg) || StaysInFlight(Reg))
        continue;
      const LiveInterval *LI = IntervalOf(Reg);
      if (!LI || LI->empty())
        continue;
      unsigned Len = InFlightValues && MRI.hasOneDef(Reg)
                         ? RematLen(MRI.getVRegDef(Reg))
                         : 0;
      // The cost of a store per definition (plus the constant's chain) and
      // a load per use not already served in flight after a definition in
      // its block, weighted by loop depth; that of rebuilding at every use;
      // and that of one pad per block entered (plus a share of the delivery
      // hops).
      unsigned SlotCost10 = 0, RematCost10 = 0; // in tenths
      for (const MachineInstr &Def : MRI.def_instructions(Reg))
        SlotCost10 += (SlotAccess10 + 10 * Len) * Weight(Def.getParent());
      for (const MachineInstr &Use : MRI.use_nodbg_instructions(Reg)) {
        bool Served = false;
        for (const MachineInstr &Def : MRI.def_instructions(Reg))
          if (Def.getParent() == Use.getParent() && Index[&Def] < Index[&Use])
            Served = true;
        unsigned W = Weight(Use.getParent());
        if (!Served)
          SlotCost10 += SlotAccess10 * W;
        RematCost10 += 10 * Len * W;
      }
      SmallVector<MachineBasicBlock *, 4> In;
      for (MachineBasicBlock &MBB : MF)
        if (LIS->isLiveInToMBB(*LI, &MBB))
          In.push_back(&MBB);
      bool FlightOK = EdgeFlight && !In.empty() && In.size() <= FlightBlocks;
      unsigned FlightCost10 = 0; // in tenths
      for (MachineBasicBlock *MBB : In)
        FlightCost10 += (10 + 3 * MBB->pred_size()) * Weight(MBB);
      // Flight cost weighted by -scry-flight-cost percent, in tenths * 100.
      uint64_t FlightScaled = (uint64_t)FlightCost10 * FlightCostPercent;
      if (Len && RematCost10 <= SlotCost10 &&
          (!FlightOK || RematCost10 * 100ull <= FlightScaled)) {
        RematRegs.insert(Reg);
        continue;
      }
      if (!FlightOK || FlightScaled > SlotCost10 * 100ull)
        continue;
      LiveInBlocks[Reg] = std::move(In);
    }
  }
  // Arrivals per block: the union over predecessors of what they deliver,
  // where a block delivers the live-ins of all its successors.
  auto ComputeArrivals = [&](DenseMap<MachineBasicBlock *, SmallVector<Register, 8>> &Arrive) {
    Arrive.clear();
    DenseMap<MachineBasicBlock *, SmallVector<Register, 8>> LiveIn;
    for (auto &KV : LiveInBlocks)
      for (MachineBasicBlock *MBB : KV.second)
        LiveIn[MBB].push_back(KV.first);
    for (MachineBasicBlock &P : MF) {
      SmallVector<Register, 8> Deliver;
      for (MachineBasicBlock *S : P.successors())
        for (Register R : LiveIn[S])
          if (!llvm::is_contained(Deliver, R))
            Deliver.push_back(R);
      for (MachineBasicBlock *S : P.successors())
        for (Register R : Deliver)
          if (!llvm::is_contained(Arrive[S], R))
            Arrive[S].push_back(R);
    }
  };
  DenseMap<MachineBasicBlock *, SmallVector<Register, 8>> Arrive;
  ComputeArrivals(Arrive);
  // Enforce the per-block cap by demoting the registers live into the most
  // blocks (they cost the most pads) until every block fits.
  for (;;) {
    MachineBasicBlock *Worst = nullptr;
    for (auto &KV : Arrive)
      if (KV.second.size() > MaxEdgeValues && (!Worst || KV.second.size() > Arrive[Worst].size()))
        Worst = KV.first;
    if (!Worst)
      break;
    Register Demote;
    unsigned Most = 0;
    for (Register R : Arrive[Worst])
      if (LiveInBlocks[R].size() >= Most) {
        Most = LiveInBlocks[R].size();
        Demote = R;
      }
    LiveInBlocks.erase(Demote);
    ComputeArrivals(Arrive);
  }
  for (auto &KV : LiveInBlocks)
    FuncInfo->setEdgeFlight(KV.first);

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
    if (MRI.reg_nodbg_empty(Reg) || LiveInBlocks.count(Reg) ||
        RematRegs.count(Reg))
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
    FuncInfo->addSlotHotness(FI, Hotness(E.Reg));
  }
  // Local objects are accessed through their ADDR_FI; count those too.
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB)
      if (MI.getOpcode() == Scry::ADDR_FI && MI.getOperand(1).isFI())
        FuncInfo->addSlotHotness(MI.getOperand(1).getIndex(),
                                 Hotness(MI.getOperand(0).getReg()));
  return true;
}
