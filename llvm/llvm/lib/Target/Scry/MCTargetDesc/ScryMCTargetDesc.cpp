//===-- ScryMCTargetDesc.cpp - Scry Target Descriptions -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryMCTargetDesc.h"
#include "ScryInstPrinter.h"
#include "ScryMCAsmInfo.h"
#include "TargetInfo/ScryTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "ScryGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "ScryGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "ScryGenRegisterInfo.inc"

using namespace llvm;

const char *Scry::getTypeCodeName(unsigned Code) {
  switch (Code) {
  case Scry::T_U8:
    return "u8";
  case Scry::T_I8:
    return "i8";
  case Scry::T_U16:
    return "u16";
  case Scry::T_I16:
    return "i16";
  case Scry::T_U32:
    return "u32";
  case Scry::T_I32:
    return "i32";
  case Scry::T_U64:
    return "u64";
  case Scry::T_I64:
    return "i64";
  case Scry::T_U128:
    return "u128";
  case Scry::T_I128:
    return "i128";
  default:
    return "?";
  }
}

const char *Scry::getSpecifierName(uint16_t S) {
  switch (S) {
  case Scry::S_B0:
    return "b0";
  case Scry::S_B1:
    return "b1";
  case Scry::S_B2:
    return "b2";
  case Scry::S_B3:
    return "b3";
  case Scry::S_PCREL_B0:
    return "pcrel_b0";
  case Scry::S_PCREL_B1:
    return "pcrel_b1";
  case Scry::S_PCREL_B2:
    return "pcrel_b2";
  case Scry::S_PCREL_B3:
    return "pcrel_b3";
  default:
    return "";
  }
}

static MCInstrInfo *createScryMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitScryMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createScryMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitScryMCRegisterInfo(X, /*RA=*/0);
  return X;
}

static MCSubtargetInfo *createScryMCSubtargetInfo(const Triple &TT,
                                                  StringRef CPU, StringRef FS) {
  std::string CPUName = std::string(CPU);
  if (CPUName.empty())
    CPUName = "generic";
  return createScryMCSubtargetInfoImpl(TT, CPUName, /*TuneCPU=*/CPUName, FS);
}

static MCAsmInfo *createScryMCAsmInfo(const MCRegisterInfo &MRI,
                                      const Triple &TT,
                                      const MCTargetOptions &Options) {
  return new ScryMCAsmInfo(TT);
}

static MCInstPrinter *createScryMCInstPrinter(const Triple &T,
                                              unsigned SyntaxVariant,
                                              const MCAsmInfo &MAI,
                                              const MCInstrInfo &MII,
                                              const MCRegisterInfo &MRI) {
  return new ScryInstPrinter(MAI, MII, MRI);
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeScryTargetMC() {
  Target &T = getTheScry32Target();
  TargetRegistry::RegisterMCAsmInfo(T, createScryMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createScryMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createScryMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createScryMCSubtargetInfo);
  TargetRegistry::RegisterMCInstPrinter(T, createScryMCInstPrinter);
  TargetRegistry::RegisterMCCodeEmitter(T, createScryMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createScryAsmBackend);
}
