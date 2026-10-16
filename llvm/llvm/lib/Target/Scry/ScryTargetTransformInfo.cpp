//===-- ScryTargetTransformInfo.cpp - Scry specific TTI -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryTargetTransformInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<unsigned> InlineMultiplier(
    "scry-inline-multiplier", cl::Hidden, cl::init(2),
    cl::desc("Multiplier of the inlining threshold for Scry"));

unsigned ScryTTIImpl::getInliningThresholdMultiplier() const {
  return InlineMultiplier;
}
