//===-- ScryMachineScheduler.h - Scry pre-emission scheduler ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A top-down list scheduling strategy for the Scry backend.  Scry has no
// registers, so the usual goals (pressure, latency) do not apply; instead the
// order of the instructions decides how early branches and loads are issued
// and how far values travel in flight before they are consumed:
//
//  1. the instructions computing a block's branch condition come first, so
//     that the jump is issued as early as possible;
//  2. loads, and the instructions computing their addresses, come next, so
//     that the loads are issued long before their results are used;
//  3. otherwise an instruction whose inputs were produced most recently is
//     preferred (short references, no hops, few values in flight), with a
//     soft limit on the number of values in flight;
//  4. ties go to the longer critical path, then the original order.
//
// Constants and addresses are rebuilt by the emitter in front of their
// consumer whatever their position, so they neither count as producers nor
// get any priority.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_SCRYMACHINESCHEDULER_H
#define LLVM_LIB_TARGET_SCRY_SCRYMACHINESCHEDULER_H

#include "llvm/CodeGen/MachineScheduler.h"

namespace llvm {

ScheduleDAGInstrs *createScryMachineScheduler(MachineSchedContext *C);

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_SCRYMACHINESCHEDULER_H
