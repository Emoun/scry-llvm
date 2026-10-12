//===-- Scry.td - Describe the Scry Target Machine ---------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level TableGen description of the Scry target.  Scry is a register-less
// architecture that passes values between instructions with forward-temporal
// references; see docs/design.md in the scry-llvm repository for how the
// backend maps LLVM's register-based machine IR onto it.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Registers, instructions
//===----------------------------------------------------------------------===//

include "ScryRegisterInfo.td"
include "ScryInstrInfo.td"

defm : RemapAllTargetPseudoPointerOperands<GR32>;

def ScryInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Processors
//===----------------------------------------------------------------------===//

def : ProcessorModel<"generic", NoSchedModel, []>;

//===----------------------------------------------------------------------===//
// Assembly printer
//===----------------------------------------------------------------------===//

def ScryInstPrinter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
  bit isMCAsmWriter = 1;
}

//===----------------------------------------------------------------------===//
// Target
//===----------------------------------------------------------------------===//

def Scry : Target {
  let InstructionSet = ScryInstrInfo;
  let AssemblyWriters = [ScryInstPrinter];
}
