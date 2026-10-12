//===-- ScryAsmBackend.cpp - Scry Assembler Backend -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ScryMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

class ScryAsmBackend : public MCAsmBackend {
  uint8_t OSABI;

public:
  ScryAsmBackend(uint8_t OSABI)
      : MCAsmBackend(llvm::endianness::little), OSABI(OSABI) {}

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createScryELFObjectWriter(OSABI);
  }
};

} // end anonymous namespace

MCFixupKindInfo ScryAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  // The 8-bit immediate of const/grow lives in bits 15..8 of the 16-bit
  // little-endian instruction word.
  const static MCFixupKindInfo Infos[Scry::NumTargetFixupKinds] = {
      // name                          offset bits flags
      {"fixup_scry_imm8_b0", 8, 8, 0},
      {"fixup_scry_imm8_b1", 8, 8, 0},
      {"fixup_scry_imm8_b2", 8, 8, 0},
      {"fixup_scry_imm8_b3", 8, 8, 0},
      {"fixup_scry_pcrel_imm8_b0", 8, 8, 0},
      {"fixup_scry_pcrel_imm8_b1", 8, 8, 0},
      {"fixup_scry_pcrel_imm8_b2", 8, 8, 0},
      {"fixup_scry_pcrel_imm8_b3", 8, 8, 0},
  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);
  assert(unsigned(Kind - FirstTargetFixupKind) < Scry::NumTargetFixupKinds &&
         "Invalid kind!");
  return Infos[Kind - FirstTargetFixupKind];
}

void ScryAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                const MCValue &Target, uint8_t *Data,
                                uint64_t Value, bool IsResolved) {
  // Symbol addresses are only known at link time, so code immediates referring
  // to symbols are always emitted as relocations.
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);

  unsigned Kind = Fixup.getKind();
  switch (Kind) {
  case FK_Data_1:
  case FK_Data_2:
  case FK_Data_4:
  case FK_Data_8: {
    unsigned Size = 1u << (Kind - FK_Data_1);
    for (unsigned i = 0; i != Size; ++i)
      Data[i] = uint8_t((Value >> (i * 8)) & 0xff);
    return;
  }
  case Scry::fixup_scry_imm8_b0:
  case Scry::fixup_scry_pcrel_imm8_b0:
    Data[1] = uint8_t(Value & 0xff);
    return;
  case Scry::fixup_scry_imm8_b1:
  case Scry::fixup_scry_pcrel_imm8_b1:
    Data[1] = uint8_t((Value >> 8) & 0xff);
    return;
  case Scry::fixup_scry_imm8_b2:
  case Scry::fixup_scry_pcrel_imm8_b2:
    Data[1] = uint8_t((Value >> 16) & 0xff);
    return;
  case Scry::fixup_scry_imm8_b3:
  case Scry::fixup_scry_pcrel_imm8_b3:
    Data[1] = uint8_t((Value >> 24) & 0xff);
    return;
  default:
    llvm_unreachable("unknown fixup kind");
  }
}

bool ScryAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                  const MCSubtargetInfo *STI) const {
  // nop is encoded as 0x4000 (little endian: 00 40).
  if (Count % 2 != 0)
    return false;
  for (uint64_t i = 0; i < Count; i += 2)
    OS.write("\x00\x40", 2);
  return true;
}

MCAsmBackend *llvm::createScryAsmBackend(const Target &T,
                                         const MCSubtargetInfo &STI,
                                         const MCRegisterInfo &MRI,
                                         const MCTargetOptions &Options) {
  return new ScryAsmBackend(ELF::ELFOSABI_NONE);
}
