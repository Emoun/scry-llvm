//===-- ScryELFObjectWriter.cpp - Scry ELF Writer -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class ScryELFObjectWriter : public MCELFObjectTargetWriter {
public:
  ScryELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_SCRY,
                                /*HasRelocationAddend=*/true) {}

  ~ScryELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override;
  bool needsRelocateWithSymbol(const MCValue &, unsigned Type) const override {
    return false;
  }
};

} // end anonymous namespace

unsigned ScryELFObjectWriter::getRelocType(const MCFixup &Fixup,
                                           const MCValue &Target,
                                           bool IsPCRel) const {
  switch ((unsigned)Fixup.getKind()) {
  case FK_Data_1:
    return ELF::R_SCRY_8;
  case FK_Data_2:
    return ELF::R_SCRY_16;
  case FK_Data_4:
    return IsPCRel ? ELF::R_SCRY_PCREL_32 : ELF::R_SCRY_32;
  case FK_Data_8:
    return ELF::R_SCRY_64;
  case Scry::fixup_scry_imm8_b0:
    return ELF::R_SCRY_IMM8_B0;
  case Scry::fixup_scry_imm8_b1:
    return ELF::R_SCRY_IMM8_B1;
  case Scry::fixup_scry_imm8_b2:
    return ELF::R_SCRY_IMM8_B2;
  case Scry::fixup_scry_imm8_b3:
    return ELF::R_SCRY_IMM8_B3;
  case Scry::fixup_scry_pcrel_imm8_b0:
    return ELF::R_SCRY_PCREL_IMM8_B0;
  case Scry::fixup_scry_pcrel_imm8_b1:
    return ELF::R_SCRY_PCREL_IMM8_B1;
  case Scry::fixup_scry_pcrel_imm8_b2:
    return ELF::R_SCRY_PCREL_IMM8_B2;
  case Scry::fixup_scry_pcrel_imm8_b3:
    return ELF::R_SCRY_PCREL_IMM8_B3;
  default:
    report_fatal_error("unsupported relocation type for Scry");
  }
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createScryELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<ScryELFObjectWriter>(OSABI);
}
