//===--- Scry.h - Declare Scry target feature support -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the Scry TargetInfo object.  Scry is a register-less,
// forward-temporal-referencing ISA; see the Scry ISA specification.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_SCRY_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_SCRY_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY ScryTargetInfo : public TargetInfo {
public:
  ScryTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    BigEndian = false;
    TLSSupported = false;
    NoAsmVariants = true;
    // Atomics are lowered to __atomic_* library calls; the simulator is
    // single-threaded so a plain implementation suffices.
    MaxAtomicPromoteWidth = MaxAtomicInlineWidth = 0;
    LongLongAlign = 64;
    DoubleAlign = LongDoubleAlign = 64;
    // Stack frames are 16-byte aligned.
    SuitableAlign = 128;
    WCharType = SignedInt;
    WIntType = UnsignedInt;
    if (Triple.getArch() == llvm::Triple::scry64) {
      LongWidth = LongAlign = 64;
      PointerWidth = PointerAlign = 64;
      SizeType = UnsignedLong;
      PtrDiffType = SignedLong;
      IntPtrType = SignedLong;
      IntMaxType = SignedLong;
      Int64Type = SignedLong;
      resetDataLayout("e-m:e-p:64:64-i64:64-i128:128-n8:16:32:64-S128");
    } else {
      LongWidth = LongAlign = 32;
      PointerWidth = PointerAlign = 32;
      SizeType = UnsignedInt;
      PtrDiffType = SignedInt;
      IntPtrType = SignedInt;
      resetDataLayout("e-m:e-p:32:32-i64:64-i128:128-n8:16:32:64-S128");
    }
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {};
  }

  bool hasFeature(StringRef Feature) const override {
    return Feature == "scry";
  }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::CharPtrBuiltinVaList;
  }

  std::string_view getClobbers() const override { return ""; }

  // The architecture has no registers, hence no register names and no
  // register constraints for inline assembly.
  ArrayRef<const char *> getGCCRegNames() const override { return {}; }
  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    return {};
  }
  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &Info) const override {
    return false;
  }
};

} // namespace targets
} // namespace clang
#endif // LLVM_CLANG_LIB_BASIC_TARGETS_SCRY_H
