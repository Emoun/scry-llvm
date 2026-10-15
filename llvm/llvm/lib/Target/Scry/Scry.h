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

/// Number of bytes (const + grows) needed to build Value as a Scale-byte
/// constant: leading zero bytes (unsigned) or sign-extension bytes (signed)
/// are redundant.
inline unsigned scryConstBytesLen(uint64_t Value, unsigned Scale,
                                  bool Signed = false) {
  unsigned N = Scale;
  while (N > 1) {
    unsigned Top = (Value >> (8 * (N - 1))) & 0xff;
    unsigned Next = (Value >> (8 * (N - 2))) & 0xff;
    bool Redundant = Signed ? (Top == 0xff && (Next & 0x80)) ||
                                  (Top == 0 && !(Next & 0x80))
                            : Top == 0;
    if (!Redundant)
      break;
    --N;
  }
  return N;
}

namespace Scry {
/// Target operand flags on symbol operands of CONST/GROW: which byte of the
/// symbol's address the immediate carries (same values as Scry::Specifier).
enum TargetFlags {
  MO_NONE = 0,
  MO_B0 = 1,
  MO_B1 = 2,
  MO_B2 = 3,
  MO_B3 = 4,
  MO_PCREL_B0 = 5, // byte of (symbol - address of this instruction)
  MO_PCREL_B1 = 6,
  MO_PCREL_B2 = 7,
  MO_PCREL_B3 = 8,
};
} // namespace Scry

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRY_H
