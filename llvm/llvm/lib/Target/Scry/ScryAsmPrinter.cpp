//===-- ScryAsmPrinter.cpp - Scry LLVM assembly writer --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to Scry assembly (or ELF objects via MC).
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/ScryInstPrinter.h"
#include "MCTargetDesc/ScryMCTargetDesc.h"
#include "Scry.h"
#include "ScryMachineFunctionInfo.h"
#include "ScryTargetMachine.h"
#include "TargetInfo/ScryTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {

class ScryAsmPrinter : public AsmPrinter {
public:
  static char ID;

  explicit ScryAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "Scry Assembly Printer"; }

  void emitInstruction(const MachineInstr *MI) override;

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override {
    return true; // inline assembly is not supported
  }

private:
  MCOperand lowerOperand(const MachineOperand &MO);
  MCOperand lowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym);
};

} // end anonymous namespace

char ScryAsmPrinter::ID = 0;

MCOperand ScryAsmPrinter::lowerSymbolOperand(const MachineOperand &MO,
                                             MCSymbol *Sym) {
  const MCExpr *Expr = MCSymbolRefExpr::create(Sym, OutContext);
  if (int64_t Off = MO.getOffset())
    Expr = MCBinaryExpr::createAdd(
        Expr, MCConstantExpr::create(Off, OutContext), OutContext);
  unsigned Flags = MO.getTargetFlags();
  if (Flags != Scry::MO_NONE)
    Expr = MCSpecifierExpr::create(Expr, (uint16_t)Flags, OutContext);
  return MCOperand::createExpr(Expr);
}

MCOperand ScryAsmPrinter::lowerOperand(const MachineOperand &MO) {
  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    return MCOperand::createImm(MO.getImm());
  case MachineOperand::MO_GlobalAddress:
    return lowerSymbolOperand(MO, getSymbol(MO.getGlobal()));
  case MachineOperand::MO_ExternalSymbol:
    return lowerSymbolOperand(MO, GetExternalSymbolSymbol(MO.getSymbolName()));
  case MachineOperand::MO_BlockAddress:
    return lowerSymbolOperand(MO, GetBlockAddressSymbol(MO.getBlockAddress()));
  case MachineOperand::MO_ConstantPoolIndex:
    return lowerSymbolOperand(MO, GetCPISymbol(MO.getIndex()));
  case MachineOperand::MO_MachineBasicBlock:
    return MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext));
  case MachineOperand::MO_RegisterMask:
    return MCOperand();
  default:
    report_fatal_error("Scry: unexpected machine operand in final code");
  }
}

void ScryAsmPrinter::emitInstruction(const MachineInstr *MI) {
  if (MI->isPseudo())
    report_fatal_error(Twine("Scry: pseudo instruction reached the printer: ") +
                       MI->getParent()->getParent()->getSubtarget().getInstrInfo()->getName(MI->getOpcode()));

  const auto *FuncInfo = MF->getInfo<ScryFunctionInfo>();
  if (const std::string *C = FuncInfo->getComment(MI))
    OutStreamer->AddComment(*C);

  MCInst Inst;
  Inst.setOpcode(MI->getOpcode());
  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp = lowerOperand(MO);
    if (MCOp.isValid())
      Inst.addOperand(MCOp);
  }
  EmitToStreamer(*OutStreamer, Inst);
}

INITIALIZE_PASS(ScryAsmPrinter, "scry-asm-printer", "Scry Assembly Printer",
                false, false)

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeScryAsmPrinter() {
  RegisterAsmPrinter<ScryAsmPrinter> X(getTheScry32Target());
}
