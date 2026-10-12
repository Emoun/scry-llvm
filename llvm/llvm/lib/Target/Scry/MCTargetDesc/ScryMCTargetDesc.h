//===-- ScryMCTargetDesc.h - Scry Target Descriptions -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides Scry-specific target descriptions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_MCTARGETDESC_SCRYMCTARGETDESC_H
#define LLVM_LIB_TARGET_SCRY_MCTARGETDESC_SCRYMCTARGETDESC_H

#include "llvm/MC/MCFixup.h"
#include "llvm/Support/DataTypes.h"
#include <memory>

namespace llvm {

class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;

MCCodeEmitter *createScryMCCodeEmitter(const MCInstrInfo &MCII,
                                       MCContext &Ctx);

MCAsmBackend *createScryAsmBackend(const Target &T, const MCSubtargetInfo &STI,
                                   const MCRegisterInfo &MRI,
                                   const MCTargetOptions &Options);

std::unique_ptr<MCObjectTargetWriter> createScryELFObjectWriter(uint8_t OSABI);

namespace Scry {

/// Target operand flags / MCSpecifierExpr specifiers: which byte of a symbol
/// value an 8-bit code immediate carries.
enum Specifier : uint16_t {
  S_None = 0,
  S_B0 = 1, // byte 0 (least significant) of the absolute address
  S_B1 = 2,
  S_B2 = 3,
  S_B3 = 4,
  S_PCREL_B0 = 5, // byte 0 of (symbol - instruction address)
  S_PCREL_B1 = 6,
  S_PCREL_B2 = 7,
  S_PCREL_B3 = 8,
};

/// Fixup kinds for the 8-bit immediate field of const/grow.
enum FixupKind {
  fixup_scry_imm8_b0 = FirstTargetFixupKind,
  fixup_scry_imm8_b1,
  fixup_scry_imm8_b2,
  fixup_scry_imm8_b3,
  fixup_scry_pcrel_imm8_b0,
  fixup_scry_pcrel_imm8_b1,
  fixup_scry_pcrel_imm8_b2,
  fixup_scry_pcrel_imm8_b3,
  LastTargetFixupKind,
  NumTargetFixupKinds = LastTargetFixupKind - FirstTargetFixupKind
};

/// Scry scalar type codes (the 4-bit 'type' and 3-bit 'ty' instruction
/// fields): even codes are unsigned, odd codes signed; code/2 is log2 of the
/// scale in bytes.
enum TypeCode {
  T_U8 = 0,
  T_I8 = 1,
  T_U16 = 2,
  T_I16 = 3,
  T_U32 = 4,
  T_I32 = 5,
  T_U64 = 6,
  T_I64 = 7,
  T_U128 = 8,
  T_I128 = 9,
};

inline unsigned typeCodeScale(unsigned Code) { return 1u << (Code / 2); }
inline unsigned unsignedTypeCode(unsigned Log2Scale) { return Log2Scale * 2; }
inline unsigned signedTypeCode(unsigned Log2Scale) { return Log2Scale * 2 + 1; }

const char *getTypeCodeName(unsigned Code);
const char *getSpecifierName(uint16_t S);

} // namespace Scry
} // namespace llvm

// Defines symbolic names for Scry registers.
#define GET_REGINFO_ENUM
#include "ScryGenRegisterInfo.inc"

// Defines symbolic names for the Scry instructions.
#define GET_INSTRINFO_ENUM
#include "ScryGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "ScryGenSubtargetInfo.inc"

#endif // LLVM_LIB_TARGET_SCRY_MCTARGETDESC_SCRYMCTARGETDESC_H
