//===-- ScryFrameLowering.cpp - Scry Frame Information --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryFrameLowering.h"
#include "ScryInstrInfo.h"
#include "ScryMachineFunctionInfo.h"
#include "ScrySubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

void ScryFrameLowering::emitPrologue(MachineFunction &MF,
                                     MachineBasicBlock &MBB) const {
  // Emitted by ScryTemporalLowering.
}

void ScryFrameLowering::emitEpilogue(MachineFunction &MF,
                                     MachineBasicBlock &MBB) const {
  // Emitted by ScryTemporalLowering.
}

MachineBasicBlock::iterator ScryFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  const ScryInstrInfo &TII = *MF.getSubtarget<ScrySubtarget>().getInstrInfo();
  unsigned Opc = I->getOpcode();
  int64_t Amount = I->getOperand(0).getImm();
  DebugLoc DL = I->getDebugLoc();
  MachineBasicBlock::iterator Next = std::next(I);
  if (Amount > 0) {
    unsigned NewOpc =
        Opc == Scry::ADJCALLSTACKDOWN ? Scry::OUT_RSRV : Scry::OUT_FREE;
    BuildMI(MBB, I, DL, TII.get(NewOpc)).addImm(Amount);
  }
  MBB.erase(I);
  return Next;
}

void ScryFrameLowering::orderFrameObjects(
    const MachineFunction &MF, SmallVectorImpl<int> &ObjectsToAllocate) const {
  const auto *FuncInfo = MF.getInfo<ScryFunctionInfo>();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  llvm::stable_sort(ObjectsToAllocate, [&](int A, int B) {
    unsigned HA = FuncInfo->getSlotHotness(A), HB = FuncInfo->getSlotHotness(B);
    if (HA != HB)
      return HA > HB;
    return MFI.getObjectSize(A) > MFI.getObjectSize(B);
  });
}

void ScryFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                             BitVector &SavedRegs,
                                             RegScavenger *RS) const {
  // Nothing is ever callee-saved: there are no registers.
  SavedRegs.clear();
}
