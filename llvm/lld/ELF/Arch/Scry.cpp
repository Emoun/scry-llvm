//===- Scry.cpp -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scry is a register-less, forward-temporal-referencing ISA with 16-bit
// instructions.  Addresses are materialised in code one byte at a time by
// const/grow instruction sequences, whose 8-bit immediate field occupies bits
// 15..8 of the little-endian instruction word.  The R_SCRY_IMM8_B<n>
// relocations fill byte <n> of the symbol value into that field.
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "Target.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {
class Scry final : public TargetInfo {
public:
  Scry(Ctx &);
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
};
} // namespace

Scry::Scry(Ctx &ctx) : TargetInfo(ctx) {
  // The "trap" instruction is encoded as all-zero bits, which is also the
  // default value of trapInstr.
  // The scryer simulator places its stack at 0x10000; keep the image well
  // clear of it.
  defaultImageBase = 0x100000;
  defaultMaxPageSize = 4096;
  defaultCommonPageSize = 4096;
}

RelExpr Scry::getRelExpr(RelType type, const Symbol &s,
                         const uint8_t *loc) const {
  switch (type) {
  case R_SCRY_PCREL_IMM8_B0:
  case R_SCRY_PCREL_IMM8_B1:
  case R_SCRY_PCREL_IMM8_B2:
  case R_SCRY_PCREL_IMM8_B3:
  case R_SCRY_PCREL_32:
    return R_PC;
  case R_SCRY_NONE:
    return R_NONE;
  default:
    return R_ABS;
  }
}

static void writeImm8(uint8_t *loc, uint8_t byte) {
  // Immediate field is bits 15..8 of the little-endian 16-bit instruction,
  // i.e. the second byte in memory.
  loc[1] = byte;
}

void Scry::relocate(uint8_t *loc, const Relocation &rel, uint64_t val) const {
  switch (rel.type) {
  case R_SCRY_8:
    checkIntUInt(ctx, loc, val, 8, rel);
    *loc = val;
    break;
  case R_SCRY_16:
    checkIntUInt(ctx, loc, val, 16, rel);
    write16le(loc, val);
    break;
  case R_SCRY_32:
  case R_SCRY_PCREL_32:
    checkIntUInt(ctx, loc, val, 32, rel);
    write32le(loc, val);
    break;
  case R_SCRY_64:
    write64le(loc, val);
    break;
  case R_SCRY_IMM8_B0:
  case R_SCRY_PCREL_IMM8_B0:
    writeImm8(loc, val & 0xff);
    break;
  case R_SCRY_IMM8_B1:
  case R_SCRY_PCREL_IMM8_B1:
    writeImm8(loc, (val >> 8) & 0xff);
    break;
  case R_SCRY_IMM8_B2:
  case R_SCRY_PCREL_IMM8_B2:
    writeImm8(loc, (val >> 16) & 0xff);
    break;
  case R_SCRY_IMM8_B3:
  case R_SCRY_PCREL_IMM8_B3:
    writeImm8(loc, (val >> 24) & 0xff);
    break;
  case R_SCRY_IMM8_B4:
    writeImm8(loc, (val >> 32) & 0xff);
    break;
  case R_SCRY_IMM8_B5:
    writeImm8(loc, (val >> 40) & 0xff);
    break;
  case R_SCRY_IMM8_B6:
    writeImm8(loc, (val >> 48) & 0xff);
    break;
  case R_SCRY_IMM8_B7:
    writeImm8(loc, (val >> 56) & 0xff);
    break;
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }
}

void elf::setScryTargetInfo(Ctx &ctx) { ctx.target.reset(new Scry(ctx)); }
