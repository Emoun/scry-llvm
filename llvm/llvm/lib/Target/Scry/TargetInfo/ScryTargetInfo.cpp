//===-- ScryTargetInfo.cpp - Scry Target Implementation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/ScryTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

Target &llvm::getTheScry32Target() {
  static Target TheScry32Target;
  return TheScry32Target;
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeScryTargetInfo() {
  RegisterTarget<Triple::scry32, /*HasJIT=*/false> X(
      getTheScry32Target(), "scry32", "Scry (32-bit)", "Scry");
}
