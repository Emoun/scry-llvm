//===-- ScryTargetMachine.h - Define TargetMachine for Scry -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRYTARGETMACHINE_H
#define LLVM_LIB_TARGET_SCRY_SCRYTARGETMACHINE_H

#include "llvm/CodeGen/MachineScheduler.h"
#include "ScrySubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include <optional>

namespace llvm {

class ScryTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  ScrySubtarget Subtarget;

public:
  ScryTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                    StringRef FS, const TargetOptions &Options,
                    std::optional<Reloc::Model> RM,
                    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                    bool JIT);

  const ScrySubtarget *getSubtargetImpl(const Function &F) const override {
    return &Subtarget;
  }

  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  TargetTransformInfo getTargetTransformInfo(const Function &F) const override;

  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }

  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                            const TargetSubtargetInfo *STI) const override;

  bool isMachineVerifierClean() const override { return false; }

  /// Values live in virtual registers (mapped to stack slots) all the way
  /// to the final lowering pass; there are no physical registers.
  bool usesPhysRegsForValues() const override { return false; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRYTARGETMACHINE_H
