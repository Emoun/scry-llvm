//===-- ScryInstrInfo.cpp - Scry Instruction Information ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryInstrInfo.h"
#include "MCTargetDesc/ScryMCTargetDesc.h"
#include "ScrySubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "ScryGenInstrInfo.inc"

ScryInstrInfo::ScryInstrInfo(const ScrySubtarget &STI)
    : ScryGenInstrInfo(STI, RI, Scry::ADJCALLSTACKDOWN, Scry::ADJCALLSTACKUP),
      RI() {}

void ScryInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI,
                                const DebugLoc &DL, Register DestReg,
                                Register SrcReg, bool KillSrc,
                                bool RenamableDest, bool RenamableSrc) const {
  // Post-RA pseudo expansion calls this for COPY instructions.  Our "copies"
  // are between virtual registers (register allocation never runs); turn them
  // into typed COPY pseudos that ScryTemporalLowering expands.
  const MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  const TargetRegisterClass *RC =
      DestReg.isVirtual() ? MRI.getRegClass(DestReg) : nullptr;
  if (!RC && SrcReg.isVirtual())
    RC = MRI.getRegClass(SrcReg);
  unsigned Opc;
  if (RC == &Scry::GR8RegClass)
    Opc = Scry::COPY_GR8;
  else if (RC == &Scry::GR16RegClass)
    Opc = Scry::COPY_GR16;
  else if (RC == &Scry::GR32RegClass)
    Opc = Scry::COPY_GR32;
  else if (RC == &Scry::GR64RegClass)
    Opc = Scry::COPY_GR64;
  else
    report_fatal_error("ScryInstrInfo::copyPhysReg: unexpected register class");
  BuildMI(MBB, MI, DL, get(Opc), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

bool ScryInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                  MachineBasicBlock *&TBB,
                                  MachineBasicBlock *&FBB,
                                  SmallVectorImpl<MachineOperand> &Cond,
                                  bool AllowModify) const {
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!isUnpredicatedTerminator(*I))
      break;
    if (!I->isBranch())
      return true;

    if (I->getOpcode() == Scry::BR) {
      if (!AllowModify) {
        TBB = I->getOperand(0).getMBB();
        continue;
      }
      // Delete anything after an unconditional branch.
      MBB.erase(std::next(I), MBB.end());
      Cond.clear();
      FBB = nullptr;
      if (MBB.isLayoutSuccessor(I->getOperand(0).getMBB())) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }
      TBB = I->getOperand(0).getMBB();
      continue;
    }

    if (I->getOpcode() != Scry::BRCOND)
      return true;

    if (Cond.empty()) {
      FBB = TBB;
      TBB = I->getOperand(2).getMBB();
      // Cond[0]: the i8 condition register, Cond[1]: 1 = branch if non-zero,
      // 0 = branch if zero.
      Cond.push_back(I->getOperand(0));
      Cond.push_back(I->getOperand(1));
      continue;
    }
    // Multiple conditional branches: not handled.
    return true;
  }
  return false;
}

unsigned ScryInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                     int *BytesRemoved) const {
  assert(!BytesRemoved && "code size not handled");
  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != Scry::BR && I->getOpcode() != Scry::BRCOND)
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  return Count;
}

unsigned ScryInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                     MachineBasicBlock *TBB,
                                     MachineBasicBlock *FBB,
                                     ArrayRef<MachineOperand> Cond,
                                     const DebugLoc &DL, int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert(!BytesAdded && "code size not handled");

  if (Cond.empty()) {
    assert(!FBB && "Unconditional branch with multiple successors!");
    BuildMI(&MBB, DL, get(Scry::BR)).addMBB(TBB);
    return 1;
  }

  assert(Cond.size() == 2 && "Scry branch conditions have two components");
  BuildMI(&MBB, DL, get(Scry::BRCOND))
      .add(Cond[0])
      .addImm(Cond[1].getImm())
      .addMBB(TBB);
  if (!FBB)
    return 1;
  BuildMI(&MBB, DL, get(Scry::BR)).addMBB(FBB);
  return 2;
}

bool ScryInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 2 && "Scry branch conditions have two components");
  Cond[1].setImm(Cond[1].getImm() ^ 1);
  return false;
}

unsigned ScryInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isPseudo())
    return 0;
  return 2;
}
