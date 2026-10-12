//===-- ScryMCCodeEmitter.cpp - Convert Scry code to machine code ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryMCTargetDesc.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "mccodeemitter"

namespace {

class ScryMCCodeEmitter : public MCCodeEmitter {
  const MCInstrInfo &MCII;
  MCContext &Ctx;

public:
  ScryMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx)
      : MCII(MCII), Ctx(Ctx) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  // TableGen'erated.
  uint64_t getBinaryCodeForInstr(const MCInst &MI,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  uint64_t getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  uint64_t getImm8OpValue(const MCInst &MI, unsigned OpNo,
                          SmallVectorImpl<MCFixup> &Fixups,
                          const MCSubtargetInfo &STI) const;
};

} // end anonymous namespace

void ScryMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                          SmallVectorImpl<char> &CB,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {
  const MCInstrDesc &Desc = MCII.get(MI.getOpcode());
  if (Desc.isPseudo())
    report_fatal_error("cannot encode a Scry pseudo instruction");
  uint64_t Bits = getBinaryCodeForInstr(MI, Fixups, STI);
  support::endian::write<uint16_t>(CB, Bits & 0xffff, llvm::endianness::little);
}

uint64_t
ScryMCCodeEmitter::getMachineOpValue(const MCInst &MI, const MCOperand &MO,
                                     SmallVectorImpl<MCFixup> &Fixups,
                                     const MCSubtargetInfo &STI) const {
  if (MO.isImm())
    return static_cast<uint64_t>(MO.getImm());
  if (MO.isReg())
    return 0;
  llvm_unreachable("unexpected operand kind");
}

uint64_t ScryMCCodeEmitter::getImm8OpValue(const MCInst &MI, unsigned OpNo,
                                           SmallVectorImpl<MCFixup> &Fixups,
                                           const MCSubtargetInfo &STI) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<uint64_t>(MO.getImm()) & 0xff;

  assert(MO.isExpr() && "imm8 operand must be an immediate or expression");
  const MCExpr *Expr = MO.getExpr();
  unsigned Kind;
  if (Expr->getKind() == MCExpr::Specifier) {
    switch (cast<MCSpecifierExpr>(Expr)->getSpecifier()) {
    case Scry::S_B0:
      Kind = Scry::fixup_scry_imm8_b0;
      break;
    case Scry::S_B1:
      Kind = Scry::fixup_scry_imm8_b1;
      break;
    case Scry::S_B2:
      Kind = Scry::fixup_scry_imm8_b2;
      break;
    case Scry::S_B3:
      Kind = Scry::fixup_scry_imm8_b3;
      break;
    case Scry::S_PCREL_B0:
      Kind = Scry::fixup_scry_pcrel_imm8_b0;
      break;
    case Scry::S_PCREL_B1:
      Kind = Scry::fixup_scry_pcrel_imm8_b1;
      break;
    case Scry::S_PCREL_B2:
      Kind = Scry::fixup_scry_pcrel_imm8_b2;
      break;
    case Scry::S_PCREL_B3:
      Kind = Scry::fixup_scry_pcrel_imm8_b3;
      break;
    default:
      Ctx.reportError(MI.getLoc(), "unknown specifier on 8-bit immediate");
      return 0;
    }
  } else {
    // A bare symbol: take its low byte.
    Kind = Scry::fixup_scry_imm8_b0;
  }
  bool IsPCRel = Kind >= Scry::fixup_scry_pcrel_imm8_b0 &&
                 Kind <= Scry::fixup_scry_pcrel_imm8_b3;
  Fixups.push_back(MCFixup::create(0, Expr, MCFixupKind(Kind), IsPCRel));
  return 0;
}

#include "ScryGenMCCodeEmitter.inc"

MCCodeEmitter *llvm::createScryMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx) {
  return new ScryMCCodeEmitter(MCII, Ctx);
}
