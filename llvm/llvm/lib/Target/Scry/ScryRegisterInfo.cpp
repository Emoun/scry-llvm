//===-- ScryRegisterInfo.cpp - Scry Register Information ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryRegisterInfo.h"
#include "MCTargetDesc/ScryMCTargetDesc.h"
#include "ScryFrameLowering.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define GET_REGINFO_TARGET_DESC
#include "ScryGenRegisterInfo.inc"

ScryRegisterInfo::ScryRegisterInfo() : ScryGenRegisterInfo(/*RA=*/0) {}

const MCPhysReg *
ScryRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  static const MCPhysReg CalleeSavedRegs[] = {0};
  return CalleeSavedRegs;
}

BitVector ScryRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  // No physical register is ever allocated; reserve them all.
  BitVector Reserved(getNumRegs());
  Reserved.set(Scry::R8_0);
  Reserved.set(Scry::R16_0);
  Reserved.set(Scry::R32_0);
  Reserved.set(Scry::R64_0);
  return Reserved;
}

bool ScryRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                           int SPAdj, unsigned FIOperandNum,
                                           RegScavenger *RS) const {
  // Frame indices only appear on ADDR_FI.  Replace the index with the byte
  // offset of the object inside the private frame; ScryTemporalLowering turns
  // it into address computations.
  MachineInstr &MI = *II;
  const MachineFrameInfo &MFI = MI.getParent()->getParent()->getFrameInfo();
  int FI = MI.getOperand(FIOperandNum).getIndex();
  int64_t Offset = MFI.getObjectOffset(FI);
  assert(Offset >= 0 && "frame objects live at non-negative offsets");
  MI.getOperand(FIOperandNum).ChangeToImmediate(Offset);
  return false;
}

Register ScryRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return Scry::R32_0;
}

const TargetRegisterClass *
ScryRegisterInfo::getPointerRegClass(unsigned Kind) const {
  return &Scry::GR32RegClass;
}
