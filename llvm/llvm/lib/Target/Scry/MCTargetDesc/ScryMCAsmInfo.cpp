//===-- ScryMCAsmInfo.cpp - Scry asm properties ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryMCAsmInfo.h"
#include "ScryMCTargetDesc.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

void ScryMCAsmInfo::anchor() {}

ScryMCAsmInfo::ScryMCAsmInfo(const Triple &TT) {
  CodePointerSize = 4;
  CalleeSaveStackSlotSize = 4;
  PrivateGlobalPrefix = ".L";
  PrivateLabelPrefix = ".L";
  // The scry_asm assembler treats ';' as the comment character.
  CommentString = ";";
  Data8bitsDirective = "\t.byte\t";
  Data16bitsDirective = "\t.short\t";
  Data32bitsDirective = "\t.long\t";
  Data64bitsDirective = "\t.quad\t";
  ZeroDirective = "\t.zero\t";
  AlignmentIsInBytes = true;
  UsesELFSectionDirectiveForBSS = true;
  SupportsDebugInformation = false;
  ExceptionsType = ExceptionHandling::None;
  UseIntegratedAssembler = true;
  UseAtForSpecifier = false;
}

void ScryMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                       const MCSpecifierExpr &Expr) const {
  uint16_t S = Expr.getSpecifier();
  StringRef Name = Scry::getSpecifierName(S);
  if (!Name.empty())
    OS << '%' << Name << '(';
  printExpr(OS, *Expr.getSubExpr());
  if (!Name.empty())
    OS << ')';
}
