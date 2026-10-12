//===-- ScryMachineFunctionInfo.h - Scry machine function info --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRYMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_SCRY_SCRYMACHINEFUNCTIONINFO_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Register.h"
#include <string>

namespace llvm {

/// Scry-specific per-function state.
class ScryFunctionInfo final : public MachineFunctionInfo {
  /// Size in bytes (multiple of 16) of the incoming stack-argument area that
  /// the caller reserved; it forms the start of this function's private frame.
  unsigned IncomingArgsSize = 0;

  /// Number of arguments delivered as operands of the call trigger (0..4).
  unsigned NumArgOperands = 0;

  /// Virtual register holding the pointer to the caller-provided buffer of
  /// variadic arguments (varargs functions only).
  Register VarArgsBufferVreg;

  /// Spill slot (frame index) of every virtual register, assigned by
  /// ScrySlotAllocation before prologue/epilogue insertion.
  DenseMap<Register, int> SpillSlots;

public:
  ScryFunctionInfo(const Function &F, const TargetSubtargetInfo *STI) {}

  MachineFunctionInfo *
  clone(BumpPtrAllocator &Allocator, MachineFunction &DestMF,
        const DenseMap<MachineBasicBlock *, MachineBasicBlock *> &Src2DstMBB)
      const override;

  unsigned getIncomingArgsSize() const { return IncomingArgsSize; }
  void setIncomingArgsSize(unsigned S) { IncomingArgsSize = S; }

  unsigned getNumArgOperands() const { return NumArgOperands; }
  void setNumArgOperands(unsigned N) { NumArgOperands = N; }

  Register getVarArgsBufferVreg() const { return VarArgsBufferVreg; }
  void setVarArgsBufferVreg(Register R) { VarArgsBufferVreg = R; }

  void setSpillSlot(Register R, int FI) { SpillSlots[R] = FI; }
  int getSpillSlot(Register R) const {
    auto It = SpillSlots.find(R);
    return It == SpillSlots.end() ? -1 : It->second;
  }
  const DenseMap<Register, int> &getSpillSlots() const { return SpillSlots; }

  /// Assembly comments attached to the first real instruction of each
  /// expanded virtual instruction (for readable .s output).
  void setComment(const MachineInstr *MI, std::string C) {
    Comments[MI] = std::move(C);
  }
  const std::string *getComment(const MachineInstr *MI) const {
    auto It = Comments.find(MI);
    return It == Comments.end() ? nullptr : &It->second;
  }

private:
  DenseMap<const MachineInstr *, std::string> Comments;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRYMACHINEFUNCTIONINFO_H
