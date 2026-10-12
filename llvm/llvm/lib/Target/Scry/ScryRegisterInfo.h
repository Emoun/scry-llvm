//===-- ScryRegisterInfo.h - Scry Register Information ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRYREGISTERINFO_H
#define LLVM_LIB_TARGET_SCRY_SCRYREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "ScryGenRegisterInfo.inc"

namespace llvm {

class ScryRegisterInfo : public ScryGenRegisterInfo {
public:
  ScryRegisterInfo();

  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;
  BitVector getReservedRegs(const MachineFunction &MF) const override;

  bool eliminateFrameIndex(MachineBasicBlock::iterator II, int SPAdj,
                           unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;

  Register getFrameRegister(const MachineFunction &MF) const override;

  const TargetRegisterClass *
  getPointerRegClass(unsigned Kind = 0) const override;

  bool requiresRegisterScavenging(const MachineFunction &MF) const override {
    return false;
  }
  bool requiresFrameIndexScavenging(const MachineFunction &MF) const override {
    return false;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRYREGISTERINFO_H
