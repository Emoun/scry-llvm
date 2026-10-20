//===-- ScryTargetTransformInfo.h - Scry specific TTI -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cost model hooks for the middle end.  A call is expensive on Scry (the
// callee address, the call, its landing slot, the argument spread, the
// frame reserves and frees), and the values in flight are lost at the
// boundary, so inlining is encouraged more than on a register machine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRYTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_SCRY_SCRYTARGETTRANSFORMINFO_H

#include "ScrySubtarget.h"
#include "ScryTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class ScryTTIImpl final : public BasicTTIImplBase<ScryTTIImpl> {
  using BaseT = BasicTTIImplBase<ScryTTIImpl>;
  using TTI = TargetTransformInfo;
  friend BaseT;

  const ScrySubtarget *ST;
  const ScryTargetLowering *TLI;

  const ScrySubtarget *getST() const { return ST; }
  const ScryTargetLowering *getTLI() const { return TLI; }

public:
  explicit ScryTTIImpl(const ScryTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  unsigned getInliningThresholdMultiplier() const override;

  void getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                               TTI::UnrollingPreferences &UP,
                               OptimizationRemarkEmitter *ORE) const override;

  unsigned getNumberOfRegisters(unsigned ClassID) const override {
    // Vectors are never legal; scalars live in flight or in frame slots.
    return ClassID == 1 ? 0 : 32;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRYTARGETTRANSFORMINFO_H
