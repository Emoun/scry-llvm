//===-- ScryFrameLowering.h - Frame info for Scry ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRYFRAMELOWERING_H
#define LLVM_LIB_TARGET_SCRY_SCRYFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class ScrySubtarget;

/// The Scry private stack frame grows upwards from the frame base: the
/// incoming stack-argument area reserved by the caller comes first, followed
/// by local objects and the spill slots of virtual registers.  The frame
/// layout is computed by the generic prologue/epilogue inserter; the actual
/// reserve/free instructions are emitted by ScryTemporalLowering, so the
/// prologue and epilogue hooks here are empty.
class ScryFrameLowering : public TargetFrameLowering {
public:
  explicit ScryFrameLowering(const ScrySubtarget &STI)
      : TargetFrameLowering(StackGrowsUp, /*StackAlignment=*/Align(16),
                            /*LocalAreaOffset=*/0,
                            /*TransientStackAlignment=*/Align(16)) {}

  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;

  /// Call frame pseudos must be lowered even when the function has no stack
  /// objects of its own.
  bool needsFrameIndexResolution(const MachineFunction &MF) const override {
    return true;
  }

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator I) const override;

  /// The most accessed objects come first, where the short slot and frame
  /// address forms reach them.
  void orderFrameObjects(const MachineFunction &MF,
                         SmallVectorImpl<int> &ObjectsToAllocate) const override;

  bool hasReservedCallFrame(const MachineFunction &MF) const override {
    return false;
  }

  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS = nullptr) const override;

protected:
  bool hasFPImpl(const MachineFunction &MF) const override { return false; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRYFRAMELOWERING_H
