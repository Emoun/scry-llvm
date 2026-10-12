//===-- ScryMCAsmInfo.h - Scry asm properties -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SCRY_MCTARGETDESC_SCRYMCASMINFO_H
#define LLVM_LIB_TARGET_SCRY_MCTARGETDESC_SCRYMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/MC/MCExpr.h"

namespace llvm {
class Triple;

class ScryMCAsmInfo : public MCAsmInfoELF {
  void anchor() override;

public:
  explicit ScryMCAsmInfo(const Triple &TT);

  void printSpecifierExpr(raw_ostream &OS,
                          const MCSpecifierExpr &Expr) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_SCRY_MCTARGETDESC_SCRYMCASMINFO_H
