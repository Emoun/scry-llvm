//===--- Scry.cpp - Implement Scry target feature support -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Scry TargetInfo object.
//
//===----------------------------------------------------------------------===//

#include "Scry.h"
#include "clang/Basic/MacroBuilder.h"

using namespace clang;
using namespace clang::targets;

void ScryTargetInfo::getTargetDefines(const LangOptions &Opts,
                                      MacroBuilder &Builder) const {
  Builder.defineMacro("__scry__");
  Builder.defineMacro("__SCRY__");
  if (getTriple().getArch() == llvm::Triple::scry64)
    Builder.defineMacro("__scry64__");
  else
    Builder.defineMacro("__scry32__");
}
