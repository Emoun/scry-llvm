//===-- ScrySubtarget.cpp - Scry Subtarget Information --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScrySubtarget.h"

#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool> ScrySched(
    "scry-sched", cl::Hidden, cl::init(true),
    cl::desc("Reorder instructions with the Scry scheduling strategy"));

bool ScrySubtarget::enableMachineScheduler() const { return ScrySched; }


#define DEBUG_TYPE "scry-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "ScryGenSubtargetInfo.inc"

ScrySubtarget::ScrySubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                             const TargetMachine &TM)
    : ScryGenSubtargetInfo(TT, CPU.empty() ? "generic" : CPU,
                           /*TuneCPU=*/CPU.empty() ? "generic" : CPU, FS),
      InstrInfo(*this), FrameLowering(*this), TLInfo(TM, *this) {
  ParseSubtargetFeatures(CPU.empty() ? "generic" : CPU,
                         CPU.empty() ? "generic" : CPU, FS);
}
