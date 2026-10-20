//===-- ScryTargetTransformInfo.cpp - Scry specific TTI -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryTargetTransformInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<unsigned> InlineMultiplier(
    "scry-inline-multiplier", cl::Hidden, cl::init(2),
    cl::desc("Multiplier of the inlining threshold for Scry"));

unsigned ScryTTIImpl::getInliningThresholdMultiplier() const {
  return InlineMultiplier;
}

// A loop iteration on Scry pays, besides its body, the landing pads and
// deliveries of its loop-carried values, the induction update, the compare
// and the jump; unrolling spreads that over more work and lengthens the body
// the jump is issued ahead of.  The doubled loop-carried state must still
// travel in flight, so only loops with few header PHIs and a small body are
// unrolled.
static cl::opt<unsigned> UnrollCount(
    "scry-unroll-count", cl::Hidden, cl::init(4),
    cl::desc("Unroll factor for loops that qualify (0 leaves unrolling to the "
             "generic heuristics)"));
static cl::opt<unsigned> UnrollMaxPhis(
    "scry-unroll-max-phis", cl::Hidden, cl::init(4),
    cl::desc("Unroll only loops with at most this many loop-carried values"));
static cl::opt<unsigned> UnrollMaxBody(
    "scry-unroll-max-body", cl::Hidden, cl::init(200),
    cl::desc("Unroll only loops with at most this many instructions"));
static cl::opt<bool> UnrollRuntime(
    "scry-unroll-runtime", cl::Hidden, cl::init(true),
    cl::desc("Also unroll loops whose trip count is only known at run time"));

void ScryTTIImpl::getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                                          TTI::UnrollingPreferences &UP,
                                          OptimizationRemarkEmitter *ORE) const {
  BaseT::getUnrollingPreferences(L, SE, UP, ORE);
  if (UnrollCount == 0 || !L->isInnermost())
    return;
  unsigned Phis = 0, Body = 0;
  for (const PHINode &P : L->getHeader()->phis()) {
    (void)P;
    ++Phis;
  }
  for (const BasicBlock *BB : L->blocks()) {
    for (const Instruction &I : *BB) {
      if (isa<CallBase>(I) && !isa<IntrinsicInst>(I))
        return; // calls empty the in-flight state anyway
      if (!isa<PHINode>(I))
        ++Body;
    }
  }
  if (Phis > UnrollMaxPhis || Body > UnrollMaxBody)
    return;
  UP.Partial = true;
  UP.Runtime = UnrollRuntime;
  UP.Count = UnrollCount;
  UP.DefaultUnrollRuntimeCount = UnrollCount;
  UP.MaxCount = UnrollCount;
  UP.PartialThreshold = std::max(UP.PartialThreshold, 4 * UP.Threshold);
}
