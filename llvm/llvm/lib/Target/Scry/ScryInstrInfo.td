//===-- ScryInstrInfo.td - Scry Instruction defs -----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// There are two families of instructions in this file:
//
//  1. "Virtual" pseudo instructions that operate on virtual registers.  These
//     are what instruction selection produces (ADD_32 $d, $a, $b etc.).  They
//     never reach the assembler: the ScryTemporalLowering pass rewrites every
//     one of them into a sequence of real instructions, keeping all values in
//     private-stack-frame slots.
//
//  2. Real Scry instructions with their 16-bit encodings.  They have no
//     register operands at all; their operands are immediates (output
//     references, stack indices, types, ...).
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Target-specific SelectionDAG nodes
//===----------------------------------------------------------------------===//

def SDT_ScryArg      : SDTypeProfile<1, 1, [SDTCisVT<1, i32>]>;
def SDT_ScryWrapper  : SDTypeProfile<1, 1, [SDTCisSameAs<0, 1>, SDTCisPtrTy<0>]>;
def SDT_ScryOutAddr  : SDTypeProfile<1, 1, [SDTCisPtrTy<0>, SDTCisVT<1, i32>]>;
def SDT_ScryRet      : SDTypeProfile<0, -1, []>;
def SDT_ScryCallSeqStart : SDCallSeqStart<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;
def SDT_ScryCallSeqEnd   : SDCallSeqEnd<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;

def ScryArg     : SDNode<"ScryISD::ARG", SDT_ScryArg>;
def ScryWrapper : SDNode<"ScryISD::WRAPPER", SDT_ScryWrapper>;
def ScryOutAddr : SDNode<"ScryISD::OUTADDR", SDT_ScryOutAddr>;
def ScryRet     : SDNode<"ScryISD::RET", SDT_ScryRet,
                         [SDNPHasChain, SDNPVariadic]>;
def ScryCallSeqStart : SDNode<"ISD::CALLSEQ_START", SDT_ScryCallSeqStart,
                              [SDNPHasChain, SDNPOutGlue]>;
def ScryCallSeqEnd   : SDNode<"ISD::CALLSEQ_END", SDT_ScryCallSeqEnd,
                              [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;

//===----------------------------------------------------------------------===//
// Operands
//===----------------------------------------------------------------------===//

// A symbol (global address, external symbol, block address) operand of a
// pseudo instruction.
def symop : Operand<i32>;
// A basic-block operand of a branch pseudo.
def brtarget : Operand<OtherVT>;

// Operands of real instructions.
def ref5    : Operand<i32> { let PrintMethod = "printRef"; }      // =>N
def ref10   : Operand<i32> { let PrintMethod = "printRef"; }      // =>N
def idx5    : Operand<i32> { let PrintMethod = "printIdx"; }      // [N]
def scrytype4   : Operand<i32> { let PrintMethod = "printType"; }     // u8/i32/...
def scrytype3   : Operand<i32> { let PrintMethod = "printType"; }
def sizecode: Operand<i32> { let PrintMethod = "printSizeType"; } // u8/u16/u32/u64
def pow2    : Operand<i32> { let PrintMethod = "printPow2"; }     // 2^N bytes
def imm8    : Operand<i32> {
  let PrintMethod = "printImm8";
  let EncoderMethod = "getImm8OpValue";
}
def simm7   : Operand<i32>;
def uimm6   : Operand<i32>;

//===----------------------------------------------------------------------===//
// Instruction formats
//===----------------------------------------------------------------------===//

class ScryInst<dag outs, dag ins, string asm, list<dag> pattern>
    : Instruction {
  let Namespace = "Scry";
  field bits<16> Inst;
  field bits<16> SoftFail = 0;
  let Size = 2;
  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = asm;
  let Pattern = pattern;
}

// Pseudo instructions on virtual registers.  Lowered by ScryTemporalLowering.
class ScryPseudo<dag outs, dag ins, list<dag> pattern = []>
    : ScryInst<outs, ins, "", pattern> {
  let isPseudo = 1;
  let isCodeGenOnly = 1;
}

//===----------------------------------------------------------------------===//
// Real instructions
//===----------------------------------------------------------------------===//

let hasSideEffects = 1 in {

// Single-output ALU: 0 ref(5) mod(3) func(3) 0001
class AluR<string mnem, bits<3> mod, bits<3> func>
    : ScryInst<(outs), (ins ref5:$ref), mnem # " $ref", []> {
  bits<5> ref;
  let Inst{15} = 0;
  let Inst{14-10} = ref;
  let Inst{9-7} = mod;
  let Inst{6-4} = func;
  let Inst{3-0} = 0b0001;
}

def EQ    : AluR<"eq",    0b000, 0b000>;
def ADDS  : AluR<"add",   0b111, 0b000>;   // saturating add (add.s)
def AND   : AluR<"and",   0b000, 0b001>;
def SUBS  : AluR<"sub",   0b111, 0b001>;   // saturating sub (sub.s)
def LT    : AluR<"lt",    0b000, 0b010>;
def GT    : AluR<"gt",    0b111, 0b010>;
def OR    : AluR<"or",    0b000, 0b011>;
def XOR   : AluR<"xor",   0b111, 0b011>;
def ISNAR : AluR<"isnar", 0b000, 0b100>;
def NARTO : AluR<"narto", 0b111, 0b100>;

// Two-output ALU operations (add, sub, sh, mul, div) with the six output
// routing modes.  mod encodings: 001 LowFirst, 010 HighFirst, 011 HighNext,
// 100 LowNext, 101 LowOnly, 110 HighOnly.
multiclass Alu2<string mnem, bits<3> func> {
  def _L   : AluR<mnem # " Low",         0b101, func>;
  def _H   : AluR<mnem # " High",        0b110, func>;
  def _LH  : AluR<mnem # " Low, High,",  0b001, func>;
  def _HL  : AluR<mnem # " High, Low,",  0b010, func>;
  // HighNext: "mnem Low =>N, High =>"; LowNext: "mnem High =>N, Low =>".
  def _LHN : ScryInst<(outs), (ins ref5:$ref), mnem # " Low $ref, High =>", []> {
    bits<5> ref;
    let Inst{15} = 0; let Inst{14-10} = ref; let Inst{9-7} = 0b011;
    let Inst{6-4} = func; let Inst{3-0} = 0b0001;
  }
  def _HLN : ScryInst<(outs), (ins ref5:$ref), mnem # " High $ref, Low =>", []> {
    bits<5> ref;
    let Inst{15} = 0; let Inst{14-10} = ref; let Inst{9-7} = 0b100;
    let Inst{6-4} = func; let Inst{3-0} = 0b0001;
  }
}

defm ADD : Alu2<"add", 0b000>;
defm SUB : Alu2<"sub", 0b001>;
defm SH  : Alu2<"sh",  0b010>;
defm MUL : Alu2<"mul", 0b100>;
defm DIV : Alu2<"div", 0b101>;

// Data flow: echo / dup.  s=1 forwards the remaining inputs to the next
// instruction.
class EchoDup<string mnem, bits<5> low, bit s, string suffix>
    : ScryInst<(outs), (ins ref5:$ref1, ref5:$ref2),
               mnem # " $ref1, $ref2" # suffix, []> {
  bits<5> ref1;
  bits<5> ref2;
  let Inst{15} = s;
  let Inst{14-10} = ref1;
  let Inst{9-5} = ref2;
  let Inst{4-0} = low;
}
def ECHO  : EchoDup<"echo", 0b01001, 0, "">;
def ECHOS : EchoDup<"echo", 0b01001, 1, ", =>">;
def DUP   : EchoDup<"dup",  0b11001, 0, "">;
def DUPS  : EchoDup<"dup",  0b11001, 1, ", =>">;

// Long echo: forwards all inputs to the instruction at the 10-bit reference.
def ECHOL : ScryInst<(outs), (ins ref10:$ref), "echo $ref", []> {
  bits<10> ref;
  let Inst{15-6} = ref;
  let Inst{5-0} = 0b010010;
}

// pick: chooses between inputs based on the first input.
def PICK : ScryInst<(outs), (ins ref5:$ref), "pick $ref", []> {
  bits<5> ref;
  let Inst{15} = 0;
  let Inst{14-10} = ref;
  let Inst{9-0} = 0b0000000010;
}

// cast: re-type (with extension/truncation) the first input.
def CAST : ScryInst<(outs), (ins scrytype4:$type, ref5:$ref), "cast $type, $ref", []> {
  bits<4> type;
  bits<5> ref;
  let Inst{15} = 1;
  let Inst{14-10} = ref;
  let Inst{9-6} = type;
  let Inst{5-0} = 0b100010;
}

// ld: load from an address given as input.
let mayLoad = 1 in
def LD : ScryInst<(outs), (ins scrytype4:$type, ref5:$ref), "ld $type, $ref", []> {
  bits<4> type;
  bits<5> ref;
  let Inst{15} = 0;
  let Inst{14-10} = ref;
  let Inst{9-6} = type;
  let Inst{5-0} = 0b100010;
}

// ld.s: load from the private stack frame slot [idx] (scaled by the type).
let mayLoad = 1 in
def LDS : ScryInst<(outs), (ins scrytype4:$type, idx5:$idx), "ld $type $idx", []> {
  bits<4> type;
  bits<5> idx;
  let Inst{15} = 0;
  let Inst{14-10} = idx;
  let Inst{9-6} = type;
  let Inst{5-0} = 0b100000;
}

// st: store the first input to the address given by the second.
let mayStore = 1 in
def ST : ScryInst<(outs), (ins), "st", []> {
  let Inst{15-0} = 0b1000000000000000;
}

// st.s: store the first input to the private stack frame slot [idx].
let mayStore = 1 in
def STS : ScryInst<(outs), (ins idx5:$idx), "st $idx", []> {
  bits<5> idx;
  let Inst{15} = 0;
  let Inst{14-10} = idx;
  let Inst{9-0} = 0b0010000000;
}

// saddr: address of private stack frame slot [idx] of the given scalar size.
def SADDR : ScryInst<(outs), (ins sizecode:$size, idx5:$idx), "saddr $size $idx", []> {
  bits<2> size;
  bits<5> idx;
  let Inst{15} = 0;
  let Inst{14-10} = idx;
  let Inst{9-8} = size;
  let Inst{7-0} = 0b01000000;
}

// Stack reservation / release.  The operand is log2 of the byte count.
class StackRes<string mnem, bit free, bit priv, string suffix>
    : ScryInst<(outs), (ins pow2:$bytes), mnem # " $bytes" # suffix, []> {
  bits<4> bytes;
  let Inst{15-14} = 0b11;
  let Inst{13-10} = bytes;
  let Inst{9} = priv;
  let Inst{8} = free;
  let Inst{7-0} = 0;
}
def RSRV  : StackRes<"rsrv", 0, 0, "">;
def RSRVP : StackRes<"rsrv", 0, 1, ", Private">;
def FREE  : StackRes<"free", 1, 0, "">;
def FREEP : StackRes<"free", 1, 1, ", Private">;

// const: produce an immediate of the given (3-bit) type.
def CONST : ScryInst<(outs), (ins scrytype3:$type, imm8:$imm), "const $type, $imm", []> {
  bits<3> type;
  bits<8> imm;
  let Inst{15-8} = imm;
  let Inst{7-5} = type;
  let Inst{4-0} = 0b10000;
}

// grow: shift the first input left by 8 bits and insert imm in the low byte.
def GROW : ScryInst<(outs), (ins imm8:$imm), "grow $imm", []> {
  bits<8> imm;
  let Inst{15-8} = imm;
  let Inst{7-0} = 0b11000000;
}

// jmp: target immediate (7-bit signed) and trigger offset (6-bit).
let isBranch = 1, isTerminator = 1 in
def JMP : ScryInst<(outs), (ins simm7:$imm, uimm6:$trig), "jmp $imm, $trig", []> {
  bits<7> imm;
  bits<6> trig;
  let Inst{15-10} = trig;
  let Inst{9-3} = imm;
  let Inst{2-0} = 0b100;
}

// call / ret with a trigger offset.
let isCall = 1 in
def CALL : ScryInst<(outs), (ins uimm6:$trig), "call $trig", []> {
  bits<6> trig;
  let Inst{15-10} = trig;
  let Inst{9-0} = 0b0110000000;
}
let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def RET : ScryInst<(outs), (ins uimm6:$trig), "ret $trig", []> {
  bits<6> trig;
  let Inst{15-10} = trig;
  let Inst{9-0} = 0b1110000000;
}

def NOP : ScryInst<(outs), (ins), "nop", []> {
  let Inst{15-0} = 0b0100000000000000;
}

def TRAP : ScryInst<(outs), (ins), "trap", [(trap)]> {
  let Inst{15-0} = 0;
}

} // hasSideEffects = 1

//===----------------------------------------------------------------------===//
// Virtual (pseudo) instructions on virtual registers
//===----------------------------------------------------------------------===//

// Incoming argument number $idx (first four arguments travel as operands of
// the call trigger).  Must stay in the entry block; expanded in the prologue.
let hasSideEffects = 1 in {
def ARG_8  : ScryPseudo<(outs GR8:$dst),  (ins i32imm:$idx), [(set GR8:$dst,  (ScryArg timm:$idx))]>;
def ARG_16 : ScryPseudo<(outs GR16:$dst), (ins i32imm:$idx), [(set GR16:$dst, (ScryArg timm:$idx))]>;
def ARG_32 : ScryPseudo<(outs GR32:$dst), (ins i32imm:$idx), [(set GR32:$dst, (ScryArg timm:$idx))]>;
def ARG_64 : ScryPseudo<(outs GR64:$dst), (ins i32imm:$idx), [(set GR64:$dst, (ScryArg timm:$idx))]>;
}

// Constants.
def CONST_8  : ScryPseudo<(outs GR8:$dst),  (ins i8imm:$imm),  [(set GR8:$dst,  imm:$imm)]>;
def CONST_16 : ScryPseudo<(outs GR16:$dst), (ins i16imm:$imm), [(set GR16:$dst, imm:$imm)]>;
def CONST_32 : ScryPseudo<(outs GR32:$dst), (ins i32imm:$imm), [(set GR32:$dst, imm:$imm)]>;
def CONST_64 : ScryPseudo<(outs GR64:$dst), (ins i64imm:$imm), [(set GR64:$dst, imm:$imm)]>;

// Absolute address of a symbol.
def ADDR_SYM : ScryPseudo<(outs GR32:$dst), (ins symop:$sym),
                          [(set GR32:$dst, (ScryWrapper tglobaladdr:$sym))]>;
def : Pat<(ScryWrapper texternalsym:$sym),  (ADDR_SYM texternalsym:$sym)>;
def : Pat<(ScryWrapper tblockaddress:$sym), (ADDR_SYM tblockaddress:$sym)>;
def : Pat<(ScryWrapper tconstpool:$sym),    (ADDR_SYM tconstpool:$sym)>;

// Address of a stack object: operand is a frame index until PEI, then the
// byte offset of the object inside the private frame.
def ADDR_FI : ScryPseudo<(outs GR32:$dst), (ins i32imm:$fi), []>;

// Address of byte offset $off inside the outgoing (shared frame) call area.
def ADDR_OUT : ScryPseudo<(outs GR32:$dst), (ins i32imm:$off),
                          [(set GR32:$dst, (ScryOutAddr timm:$off))]>;

// Binary arithmetic.
multiclass BinOps<RegisterClass RC> {
  def ADD_#RC  : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (add  RC:$a, RC:$b))]>;
  def SUB_#RC  : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (sub  RC:$a, RC:$b))]>;
  def MUL_#RC  : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (mul  RC:$a, RC:$b))]>;
  def AND_#RC  : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (and  RC:$a, RC:$b))]>;
  def OR_#RC   : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (or   RC:$a, RC:$b))]>;
  def XOR_#RC  : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (xor  RC:$a, RC:$b))]>;
  def UDIV_#RC : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (udiv RC:$a, RC:$b))]>;
  def UREM_#RC : ScryPseudo<(outs RC:$dst), (ins RC:$a, RC:$b), [(set RC:$dst, (urem RC:$a, RC:$b))]>;
  def SHL_#RC  : ScryPseudo<(outs RC:$dst), (ins RC:$a, GR32:$n), [(set RC:$dst, (shl RC:$a, GR32:$n))]>;
  def LSHR_#RC : ScryPseudo<(outs RC:$dst), (ins RC:$a, GR32:$n), [(set RC:$dst, (srl RC:$a, GR32:$n))]>;
  def ASHR_#RC : ScryPseudo<(outs RC:$dst), (ins RC:$a, GR32:$n), [(set RC:$dst, (sra RC:$a, GR32:$n))]>;
}
defm "" : BinOps<GR8>;
defm "" : BinOps<GR16>;
defm "" : BinOps<GR32>;
defm "" : BinOps<GR64>;

// Comparisons produce an i8 boolean (0 or 1).
multiclass CmpOps<RegisterClass RC> {
  def CMP_EQ_#RC  : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETEQ))]>;
  def CMP_NE_#RC  : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETNE))]>;
  def CMP_ULT_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETULT))]>;
  def CMP_ULE_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETULE))]>;
  def CMP_UGT_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETUGT))]>;
  def CMP_UGE_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETUGE))]>;
  def CMP_SLT_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETLT))]>;
  def CMP_SLE_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETLE))]>;
  def CMP_SGT_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETGT))]>;
  def CMP_SGE_#RC : ScryPseudo<(outs GR8:$dst), (ins RC:$a, RC:$b), [(set GR8:$dst, (setcc RC:$a, RC:$b, SETGE))]>;
}
defm "" : CmpOps<GR8>;
defm "" : CmpOps<GR16>;
defm "" : CmpOps<GR32>;
defm "" : CmpOps<GR64>;

// Register-to-register copies (produced from COPY by copyPhysReg).
multiclass CopyOps<RegisterClass RC> {
  def COPY_#RC : ScryPseudo<(outs RC:$dst), (ins RC:$src), []>;
}
defm "" : CopyOps<GR8>;
defm "" : CopyOps<GR16>;
defm "" : CopyOps<GR32>;
defm "" : CopyOps<GR64>;

// Select.
multiclass SelOps<RegisterClass RC> {
  def SELECT_#RC : ScryPseudo<(outs RC:$dst), (ins GR8:$c, RC:$a, RC:$b),
                              [(set RC:$dst, (select GR8:$c, RC:$a, RC:$b))]>;
}
defm "" : SelOps<GR8>;
defm "" : SelOps<GR16>;
defm "" : SelOps<GR32>;
defm "" : SelOps<GR64>;

// Conversions.
class ExtOp<RegisterClass DRC, RegisterClass SRC, SDNode node, ValueType vt>
    : ScryPseudo<(outs DRC:$dst), (ins SRC:$src), [(set DRC:$dst, (vt (node SRC:$src)))]>;

def ZEXT_8_16  : ExtOp<GR16, GR8,  zext, i16>;
def ZEXT_8_32  : ExtOp<GR32, GR8,  zext, i32>;
def ZEXT_8_64  : ExtOp<GR64, GR8,  zext, i64>;
def ZEXT_16_32 : ExtOp<GR32, GR16, zext, i32>;
def ZEXT_16_64 : ExtOp<GR64, GR16, zext, i64>;
def ZEXT_32_64 : ExtOp<GR64, GR32, zext, i64>;
def SEXT_8_16  : ExtOp<GR16, GR8,  sext, i16>;
def SEXT_8_32  : ExtOp<GR32, GR8,  sext, i32>;
def SEXT_8_64  : ExtOp<GR64, GR8,  sext, i64>;
def SEXT_16_32 : ExtOp<GR32, GR16, sext, i32>;
def SEXT_16_64 : ExtOp<GR64, GR16, sext, i64>;
def SEXT_32_64 : ExtOp<GR64, GR32, sext, i64>;
def TRUNC_16_8  : ExtOp<GR8,  GR16, trunc, i8>;
def TRUNC_32_8  : ExtOp<GR8,  GR32, trunc, i8>;
def TRUNC_64_8  : ExtOp<GR8,  GR64, trunc, i8>;
def TRUNC_32_16 : ExtOp<GR16, GR32, trunc, i16>;
def TRUNC_64_16 : ExtOp<GR16, GR64, trunc, i16>;
def TRUNC_64_32 : ExtOp<GR32, GR64, trunc, i32>;

// anyext is just zext.
def : Pat<(i16 (anyext GR8:$s)),  (ZEXT_8_16 GR8:$s)>;
def : Pat<(i32 (anyext GR8:$s)),  (ZEXT_8_32 GR8:$s)>;
def : Pat<(i64 (anyext GR8:$s)),  (ZEXT_8_64 GR8:$s)>;
def : Pat<(i32 (anyext GR16:$s)), (ZEXT_16_32 GR16:$s)>;
def : Pat<(i64 (anyext GR16:$s)), (ZEXT_16_64 GR16:$s)>;
def : Pat<(i64 (anyext GR32:$s)), (ZEXT_32_64 GR32:$s)>;

// sign_extend_inreg: operand is the source width in bits (8, 16 or 32).
def SEXTINREG_16 : ScryPseudo<(outs GR16:$dst), (ins GR16:$src, i32imm:$bits), []>;
def SEXTINREG_32 : ScryPseudo<(outs GR32:$dst), (ins GR32:$src, i32imm:$bits), []>;
def SEXTINREG_64 : ScryPseudo<(outs GR64:$dst), (ins GR64:$src, i32imm:$bits), []>;
def : Pat<(sext_inreg GR16:$s, i8),  (SEXTINREG_16 GR16:$s, 8)>;
def : Pat<(sext_inreg GR32:$s, i8),  (SEXTINREG_32 GR32:$s, 8)>;
def : Pat<(sext_inreg GR32:$s, i16), (SEXTINREG_32 GR32:$s, 16)>;
def : Pat<(sext_inreg GR64:$s, i8),  (SEXTINREG_64 GR64:$s, 8)>;
def : Pat<(sext_inreg GR64:$s, i16), (SEXTINREG_64 GR64:$s, 16)>;
def : Pat<(sext_inreg GR64:$s, i32), (SEXTINREG_64 GR64:$s, 32)>;

// Memory.  Addresses are i32 (scry32).
let mayLoad = 1 in {
def LD_8  : ScryPseudo<(outs GR8:$dst),  (ins GR32:$addr), [(set GR8:$dst,  (load GR32:$addr))]>;
def LD_16 : ScryPseudo<(outs GR16:$dst), (ins GR32:$addr), [(set GR16:$dst, (load GR32:$addr))]>;
def LD_32 : ScryPseudo<(outs GR32:$dst), (ins GR32:$addr), [(set GR32:$dst, (load GR32:$addr))]>;
def LD_64 : ScryPseudo<(outs GR64:$dst), (ins GR32:$addr), [(set GR64:$dst, (load GR32:$addr))]>;
}
let mayStore = 1 in {
def ST_8  : ScryPseudo<(outs), (ins GR8:$val,  GR32:$addr), [(store GR8:$val,  GR32:$addr)]>;
def ST_16 : ScryPseudo<(outs), (ins GR16:$val, GR32:$addr), [(store GR16:$val, GR32:$addr)]>;
def ST_32 : ScryPseudo<(outs), (ins GR32:$val, GR32:$addr), [(store GR32:$val, GR32:$addr)]>;
def ST_64 : ScryPseudo<(outs), (ins GR64:$val, GR32:$addr), [(store GR64:$val, GR32:$addr)]>;
}

// Extending loads and truncating stores.
def : Pat<(i16 (zextloadi8 GR32:$a)),  (ZEXT_8_16 (LD_8 GR32:$a))>;
def : Pat<(i32 (zextloadi8 GR32:$a)),  (ZEXT_8_32 (LD_8 GR32:$a))>;
def : Pat<(i64 (zextloadi8 GR32:$a)),  (ZEXT_8_64 (LD_8 GR32:$a))>;
def : Pat<(i32 (zextloadi16 GR32:$a)), (ZEXT_16_32 (LD_16 GR32:$a))>;
def : Pat<(i64 (zextloadi16 GR32:$a)), (ZEXT_16_64 (LD_16 GR32:$a))>;
def : Pat<(i64 (zextloadi32 GR32:$a)), (ZEXT_32_64 (LD_32 GR32:$a))>;
def : Pat<(i16 (sextloadi8 GR32:$a)),  (SEXT_8_16 (LD_8 GR32:$a))>;
def : Pat<(i32 (sextloadi8 GR32:$a)),  (SEXT_8_32 (LD_8 GR32:$a))>;
def : Pat<(i64 (sextloadi8 GR32:$a)),  (SEXT_8_64 (LD_8 GR32:$a))>;
def : Pat<(i32 (sextloadi16 GR32:$a)), (SEXT_16_32 (LD_16 GR32:$a))>;
def : Pat<(i64 (sextloadi16 GR32:$a)), (SEXT_16_64 (LD_16 GR32:$a))>;
def : Pat<(i64 (sextloadi32 GR32:$a)), (SEXT_32_64 (LD_32 GR32:$a))>;
def : Pat<(i16 (extloadi8 GR32:$a)),   (ZEXT_8_16 (LD_8 GR32:$a))>;
def : Pat<(i32 (extloadi8 GR32:$a)),   (ZEXT_8_32 (LD_8 GR32:$a))>;
def : Pat<(i64 (extloadi8 GR32:$a)),   (ZEXT_8_64 (LD_8 GR32:$a))>;
def : Pat<(i32 (extloadi16 GR32:$a)),  (ZEXT_16_32 (LD_16 GR32:$a))>;
def : Pat<(i64 (extloadi16 GR32:$a)),  (ZEXT_16_64 (LD_16 GR32:$a))>;
def : Pat<(i64 (extloadi32 GR32:$a)),  (ZEXT_32_64 (LD_32 GR32:$a))>;
def : Pat<(truncstorei8 GR16:$v, GR32:$a),  (ST_8 (TRUNC_16_8 GR16:$v), GR32:$a)>;
def : Pat<(truncstorei8 GR32:$v, GR32:$a),  (ST_8 (TRUNC_32_8 GR32:$v), GR32:$a)>;
def : Pat<(truncstorei8 GR64:$v, GR32:$a),  (ST_8 (TRUNC_64_8 GR64:$v), GR32:$a)>;
def : Pat<(truncstorei16 GR32:$v, GR32:$a), (ST_16 (TRUNC_32_16 GR32:$v), GR32:$a)>;
def : Pat<(truncstorei16 GR64:$v, GR32:$a), (ST_16 (TRUNC_64_16 GR64:$v), GR32:$a)>;
def : Pat<(truncstorei32 GR64:$v, GR32:$a), (ST_32 (TRUNC_64_32 GR64:$v), GR32:$a)>;

// Control flow.
let isBranch = 1, isTerminator = 1 in {
let isBarrier = 1 in
def BR : ScryPseudo<(outs), (ins brtarget:$dst), [(br bb:$dst)]>;
// Branch to $dst if $cond is non-zero ($pol = 1) or zero ($pol = 0).
def BRCOND : ScryPseudo<(outs), (ins GR8:$cond, i32imm:$pol, brtarget:$dst),
                        []>;
}
def : Pat<(brcond GR8:$cond, bb:$dst), (BRCOND GR8:$cond, 1, bb:$dst)>;
def : Pat<(brcond (xor GR8:$cond, 1), bb:$dst), (BRCOND GR8:$cond, 0, bb:$dst)>;

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def RETURN : ScryPseudo<(outs), (ins variable_ops), [(ScryRet)]>;

// Calls.  $callee is a symbol (CALL_*) or an address in a register (CALLI_*).
// Arguments follow as variable operands.  ADJCALLSTACKDOWN/UP bracket the
// call and carry the outgoing stack-argument area size.  The pseudos are
// deliberately not flagged isCall: the generic machine scheduler never moves
// anything across a call, but on Scry nothing is clobbered by one, and
// issuing the branch condition or a load before a call is exactly what the
// scheduler is for.  hasSideEffects keeps memory accesses ordered with them.
let hasSideEffects = 1 in {
def CALL_V   : ScryPseudo<(outs), (ins symop:$callee, variable_ops), []>;
def CALL_8   : ScryPseudo<(outs GR8:$dst),  (ins symop:$callee, variable_ops), []>;
def CALL_16  : ScryPseudo<(outs GR16:$dst), (ins symop:$callee, variable_ops), []>;
def CALL_32  : ScryPseudo<(outs GR32:$dst), (ins symop:$callee, variable_ops), []>;
def CALL_64  : ScryPseudo<(outs GR64:$dst), (ins symop:$callee, variable_ops), []>;
def CALLI_V  : ScryPseudo<(outs), (ins GR32:$callee, variable_ops), []>;
def CALLI_8  : ScryPseudo<(outs GR8:$dst),  (ins GR32:$callee, variable_ops), []>;
def CALLI_16 : ScryPseudo<(outs GR16:$dst), (ins GR32:$callee, variable_ops), []>;
def CALLI_32 : ScryPseudo<(outs GR32:$dst), (ins GR32:$callee, variable_ops), []>;
def CALLI_64 : ScryPseudo<(outs GR64:$dst), (ins GR32:$callee, variable_ops), []>;
}

let hasSideEffects = 1 in {
def ADJCALLSTACKDOWN : ScryPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                                  [(ScryCallSeqStart timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP   : ScryPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2),
                                  [(ScryCallSeqEnd timm:$amt1, timm:$amt2)]>;
// Post-PEI forms: reserve/free $amt bytes in the shared (outgoing) frame.
def OUT_RSRV : ScryPseudo<(outs), (ins i32imm:$amt), []>;
def OUT_FREE : ScryPseudo<(outs), (ins i32imm:$amt), []>;
}
