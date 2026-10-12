//===-- Scry.h - Top-level interface for Scry representation ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in the LLVM
// Scry backend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRY_H
#define LLVM_LIB_TARGET_SCRY_SCRY_H

#include "MCTargetDesc/ScryMCTargetDesc.h"
#include "llvm/Support/CodeGen.h"

namespace llvm {

class FunctionPass;
class PassRegistry;
class ScryTargetMachine;

FunctionPass *createScryISelDag(ScryTargetMachine &TM,
                                CodeGenOptLevel OptLevel);
FunctionPass *createScryArgHoistingPass();
FunctionPass *createScrySlotAllocationPass();
FunctionPass *createScryTemporalLoweringPass();

void initializeScryDAGToDAGISelLegacyPass(PassRegistry &);
void initializeScryArgHoistingPass(PassRegistry &);
void initializeScrySlotAllocationPass(PassRegistry &);
void initializeScryTemporalLoweringPass(PassRegistry &);
void initializeScryAsmPrinterPass(PassRegistry &);

namespace Scry {
/// Target operand flags on symbol operands of CONST/GROW: which byte of the
/// symbol's address the immediate carries (same values as Scry::Specifier).
enum TargetFlags {
  MO_NONE = 0,
  MO_B0 = 1,
  MO_B1 = 2,
  MO_B2 = 3,
  MO_B3 = 4,
};
} // namespace Scry

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRY_H
