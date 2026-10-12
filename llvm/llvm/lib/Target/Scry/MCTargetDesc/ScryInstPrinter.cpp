//===-- ScryInstPrinter.cpp - Convert Scry MCInst to asm syntax -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Prints Scry instructions in the syntax understood by the reference
// scry_asm assembler, e.g. "add Low =>3", "ld u32 [2]", "const u8, 5".
//
//===----------------------------------------------------------------------===//

#include "ScryInstPrinter.h"
#include "ScryMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

#include "ScryGenAsmWriter.inc"

void ScryInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg);
}

void ScryInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                StringRef Annot, const MCSubtargetInfo &STI,
                                raw_ostream &O) {
  printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void ScryInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                   raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm())
    O << MO.getImm();
  else if (MO.isExpr())
    MAI.printExpr(O, *MO.getExpr());
  else if (MO.isReg())
    O << getRegisterName(MO.getReg());
  else
    llvm_unreachable("unknown operand kind");
}

void ScryInstPrinter::printRef(const MCInst *MI, unsigned OpNo,
                               raw_ostream &O) {
  O << "=>" << MI->getOperand(OpNo).getImm();
}

void ScryInstPrinter::printIdx(const MCInst *MI, unsigned OpNo,
                               raw_ostream &O) {
  O << '[' << MI->getOperand(OpNo).getImm() << ']';
}

void ScryInstPrinter::printType(const MCInst *MI, unsigned OpNo,
                                raw_ostream &O) {
  O << Scry::getTypeCodeName(MI->getOperand(OpNo).getImm());
}

void ScryInstPrinter::printSizeType(const MCInst *MI, unsigned OpNo,
                                    raw_ostream &O) {
  // saddr size code: log2 of the scalar size in bytes.
  O << Scry::getTypeCodeName(Scry::unsignedTypeCode(MI->getOperand(OpNo).getImm()));
}

void ScryInstPrinter::printPow2(const MCInst *MI, unsigned OpNo,
                                raw_ostream &O) {
  O << (1ull << MI->getOperand(OpNo).getImm());
}

void ScryInstPrinter::printImm8(const MCInst *MI, unsigned OpNo,
                                raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm())
    O << (MO.getImm() & 0xff);
  else
    MAI.printExpr(O, *MO.getExpr());
}
