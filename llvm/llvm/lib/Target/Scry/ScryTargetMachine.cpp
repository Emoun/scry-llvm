//===-- ScryTargetMachine.cpp - Define TargetMachine for Scry -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryTargetMachine.h"
#include "ScryMachineScheduler.h"
#include "ScryTargetTransformInfo.h"
#include "Scry.h"
#include "ScryMachineFunctionInfo.h"
#include "TargetInfo/ScryTargetInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Transforms/Scalar.h"
#include <optional>

using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeScryTarget() {
  RegisterTargetMachine<ScryTargetMachine> X(getTheScry32Target());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeScryDAGToDAGISelLegacyPass(PR);
  initializeScryArgHoistingPass(PR);
  initializeScrySlotAllocationPass(PR);
  initializeScryTemporalLoweringPass(PR);
  initializeScryAsmPrinterPass(PR);
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

ScryTargetMachine::ScryTargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     const TargetOptions &Options,
                                     std::optional<Reloc::Model> RM,
                                     std::optional<CodeModel::Model> CM,
                                     CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
                               getEffectiveRelocModel(RM),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT, CPU, FS, *this) {
  initAsmInfo();
}

MachineFunctionInfo *ScryTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return ScryFunctionInfo::create<ScryFunctionInfo>(Allocator, F, STI);
}

namespace {

class ScryPassConfig : public TargetPassConfig {
public:
  ScryPassConfig(ScryTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  ScryTargetMachine &getScryTargetMachine() const {
    return getTM<ScryTargetMachine>();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  void addPostRegAlloc() override;
  void addPreEmitPass2() override;

  // Register allocation never runs: values stay in virtual registers until
  // ScryTemporalLowering assigns them stack slots.
  FunctionPass *createTargetRegisterAllocator(bool) override { return nullptr; }
  bool addRegAssignAndRewriteFast() override { return false; }
  bool addRegAssignAndRewriteOptimized() override { return false; }
};

} // end anonymous namespace

ScheduleDAGInstrs *
ScryTargetMachine::createMachineScheduler(MachineSchedContext *C) const {
  return createScryMachineScheduler(C);
}

TargetTransformInfo
ScryTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<ScryTTIImpl>(this, F));
}

TargetPassConfig *ScryTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new ScryPassConfig(*this, PM);
}

void ScryPassConfig::addIRPasses() {
  // Atomics become __atomic_* library calls (single-threaded simulator).
  addPass(createAtomicExpandLegacyPass());
  // indirectbr is turned into a switch over block numbers.
  addPass(createIndirectBrExpandPass());
  TargetPassConfig::addIRPasses();
}

bool ScryPassConfig::addInstSelector() {
  addPass(createScryISelDag(getScryTargetMachine(), getOptLevel()));
  return false;
}

void ScryPassConfig::addPostRegAlloc() {
  // These passes require the NoVRegs property, which we never reach.
  disablePass(&MachineLateInstrsCleanupID);
  disablePass(&MachineCopyPropagationID);
  disablePass(&PostRAMachineSinkingID);
  disablePass(&PostRASchedulerID);
  disablePass(&FuncletLayoutID);
  disablePass(&StackMapLivenessID);
  disablePass(&PatchableFunctionID);
  disablePass(&ShrinkWrapID);
  disablePass(&RemoveLoadsIntoFakeUsesID);
  disablePass(&LiveDebugValuesID);

  // Arguments are delivered at function entry whatever the scheduler did with
  // the ARG pseudos, so pin them there before liveness is computed.
  addPass(createScryArgHoistingPass());
  // Give every virtual register a frame slot before the frame is finalized.
  addPass(createScrySlotAllocationPass());
  TargetPassConfig::addPostRegAlloc();
}

void ScryPassConfig::addPreEmitPass2() {
  // Must be the very last pass: it fixes the instruction layout.
  addPass(createScryTemporalLoweringPass());
}
