//===-- ScryRegisterInfo.td - Scry Register defs -----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scry has no architectural registers.  Code generation nevertheless uses
// LLVM's virtual registers: every value computed by a "virtual" (pseudo)
// instruction lives in a virtual register of one of the classes below, and
// the late ScryTemporalLowering pass maps every virtual register to a private
// stack-frame slot.  Register allocation never runs, so the single physical
// register per class only exists to satisfy TableGen.
//
//===----------------------------------------------------------------------===//

class ScryReg<string n> : Register<n> {
  let Namespace = "Scry";
}

// Placeholder physical registers (never allocated).
def R8_0  : ScryReg<"%r8">;
def R16_0 : ScryReg<"%r16">;
def R32_0 : ScryReg<"%r32">;
def R64_0 : ScryReg<"%r64">;

// One register class per legal integer type.  i1 values are promoted to i8.
def GR8  : RegisterClass<"Scry", [i8],  8,  (add R8_0)>;
def GR16 : RegisterClass<"Scry", [i16], 16, (add R16_0)>;
def GR32 : RegisterClass<"Scry", [i32], 32, (add R32_0)>;
def GR64 : RegisterClass<"Scry", [i64], 64, (add R64_0)>;
