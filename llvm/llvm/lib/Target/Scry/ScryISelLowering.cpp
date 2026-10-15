//===-- ScryISelLowering.cpp - Scry DAG Lowering Implementation -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that Scry uses to lower LLVM code into a
// selection DAG.
//
// Calling convention (see the Scry ISA specification, section 3.1):
//  * the first four scalar arguments are delivered as operands of the call
//    trigger, in order;
//  * further arguments, and all variadic arguments, are placed on the stack in
//    the area the caller reserves in its shared frame (which becomes the
//    callee's private frame), each at the next offset aligned to its size;
//  * up to four scalar results travel back through the return trigger.
// Aggregates are never passed or returned by value (clang passes pointers).
//
//===----------------------------------------------------------------------===//

#include "ScryISelLowering.h"
#include "Scry.h"
#include "ScryMachineFunctionInfo.h"
#include "ScrySubtarget.h"
#include "ScryTargetMachine.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "scry-lower"

/// Maximum number of values delivered through a call or return trigger.
static const unsigned MaxTriggerOperands = 4;

/// Report a fatal diagnostic attached to the function being lowered.
static void fail(const SDLoc &DL, SelectionDAG &DAG, const Twine &Msg) {
  MachineFunction &MF = DAG.getMachineFunction();
  DAG.getContext()->diagnose(
      DiagnosticInfoUnsupported(MF.getFunction(), Msg, DL.getDebugLoc()));
}

ScryTargetLowering::ScryTargetLowering(const TargetMachine &TM,
                                       const ScrySubtarget &STI)
    : TargetLowering(TM, STI), Subtarget(STI) {
  addRegisterClass(MVT::i8, &Scry::GR8RegClass);
  addRegisterClass(MVT::i16, &Scry::GR16RegClass);
  addRegisterClass(MVT::i32, &Scry::GR32RegClass);
  addRegisterClass(MVT::i64, &Scry::GR64RegClass);
  computeRegisterProperties(Subtarget.getRegisterInfo());

  setBooleanContents(ZeroOrOneBooleanContent);
  setMinFunctionAlignment(Align(2));
  setPrefFunctionAlignment(Align(2));
  setMaxAtomicSizeInBitsSupported(0);
  setStackPointerRegisterToSaveRestore(Scry::R32_0);

  for (MVT VT : {MVT::i8, MVT::i16, MVT::i32, MVT::i64}) {
    // Operations with no native equivalent are expanded by the legalizer.
    setOperationAction({ISD::MULHU, ISD::MULHS, ISD::SMUL_LOHI, ISD::UMUL_LOHI,
                        ISD::SDIVREM, ISD::UDIVREM, ISD::ROTL, ISD::ROTR,
                        ISD::BSWAP, ISD::CTLZ, ISD::CTTZ, ISD::CTPOP,
                        ISD::CTLZ_ZERO_UNDEF, ISD::CTTZ_ZERO_UNDEF,
                        ISD::BITREVERSE, ISD::ABS, ISD::SMIN, ISD::SMAX,
                        ISD::UMIN, ISD::UMAX, ISD::SADDO, ISD::UADDO, ISD::SSUBO,
                        ISD::USUBO, ISD::SMULO, ISD::UMULO, ISD::SADDSAT,
                        ISD::UADDSAT, ISD::SSUBSAT, ISD::USUBSAT, ISD::SHL_PARTS,
                        ISD::SRA_PARTS, ISD::SRL_PARTS, ISD::SELECT_CC,
                        ISD::BR_CC, ISD::SETCCCARRY, ISD::UADDO_CARRY,
                        ISD::USUBO_CARRY, ISD::SADDO_CARRY, ISD::SSUBO_CARRY,
                        ISD::FSHL, ISD::FSHR},
                       VT, Expand);

    // Signed division is Euclidean on Scry; C needs truncating division, so
    // use the compiler-rt routines.
    if (VT == MVT::i8 || VT == MVT::i16)
      setOperationAction({ISD::SDIV, ISD::SREM}, VT, Promote);
    else
      setOperationAction({ISD::SDIV, ISD::SREM}, VT, LibCall);

    // i1 is never legal.
    setLoadExtAction({ISD::EXTLOAD, ISD::ZEXTLOAD, ISD::SEXTLOAD}, VT, MVT::i1,
                     Promote);
    setTruncStoreAction(VT, MVT::i1, Expand);
  }
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);

  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::ExternalSymbol, MVT::i32, Custom);
  setOperationAction(ISD::BlockAddress, MVT::i32, Custom);
  setOperationAction(ISD::ConstantPool, MVT::i32, Custom);
  setOperationAction(ISD::JumpTable, MVT::i32, Custom);

  setOperationAction(ISD::BR_JT, MVT::Other, Expand);
  setOperationAction(ISD::BRIND, MVT::Other, Expand);

  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction({ISD::VAARG, ISD::VACOPY, ISD::VAEND}, MVT::Other, Expand);

  setOperationAction({ISD::STACKSAVE, ISD::STACKRESTORE}, MVT::Other, Custom);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Custom);

  setOperationAction(ISD::TRAP, MVT::Other, Legal);
  setOperationAction(ISD::DEBUGTRAP, MVT::Other, Expand);

  // Every memory access costs several instructions; let LLVM lower memcpy
  // and friends to library calls early.
  MaxStoresPerMemcpy = MaxStoresPerMemcpyOptSize = 4;
  MaxStoresPerMemset = MaxStoresPerMemsetOptSize = 4;
  MaxStoresPerMemmove = MaxStoresPerMemmoveOptSize = 4;
}

const char *ScryTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch ((ScryISD::NodeType)Opcode) {
  case ScryISD::FIRST_NUMBER:
    break;
  case ScryISD::ARG:
    return "ScryISD::ARG";
  case ScryISD::WRAPPER:
    return "ScryISD::WRAPPER";
  case ScryISD::OUTADDR:
    return "ScryISD::OUTADDR";
  case ScryISD::RET:
    return "ScryISD::RET";
  case ScryISD::CALL:
    return "ScryISD::CALL";
  }
  return nullptr;
}

EVT ScryTargetLowering::getSetCCResultType(const DataLayout &DL,
                                           LLVMContext &Context, EVT VT) const {
  if (VT.isVector())
    return VT.changeVectorElementTypeToInteger();
  return MVT::i8;
}

bool ScryTargetLowering::isLegalAddressingMode(const DataLayout &DL,
                                               const AddrMode &AM, Type *Ty,
                                               unsigned AS,
                                               Instruction *I) const {
  // Only plain base addresses are "free": every offset or scaled index costs
  // explicit arithmetic.
  return AM.BaseOffs == 0 && AM.Scale == 0 && !AM.BaseGV;
}

//===----------------------------------------------------------------------===//
// Custom lowering
//===----------------------------------------------------------------------===//

SDValue ScryTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress:
    return LowerGlobalAddress(Op, DAG);
  case ISD::ExternalSymbol:
    return LowerExternalSymbol(Op, DAG);
  case ISD::BlockAddress:
    return LowerBlockAddress(Op, DAG);
  case ISD::VASTART:
    return LowerVASTART(Op, DAG);
  case ISD::ConstantPool:
    return LowerConstantPool(Op, DAG);
  case ISD::JumpTable:
    report_fatal_error("Scry: jump tables are not supported");
  case ISD::STACKSAVE:
  case ISD::STACKRESTORE:
  case ISD::DYNAMIC_STACKALLOC:
    report_fatal_error(
        "Scry: dynamic stack allocation (alloca with a variable size) is not "
        "supported");
  default:
    llvm_unreachable("unimplemented operand");
  }
}

SDValue ScryTargetLowering::LowerGlobalAddress(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc DL(Op);
  auto *N = cast<GlobalAddressSDNode>(Op);
  EVT PtrVT = Op.getValueType();
  SDValue Addr =
      DAG.getTargetGlobalAddress(N->getGlobal(), DL, PtrVT, N->getOffset());
  return DAG.getNode(ScryISD::WRAPPER, DL, PtrVT, Addr);
}

SDValue ScryTargetLowering::LowerExternalSymbol(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  auto *N = cast<ExternalSymbolSDNode>(Op);
  EVT PtrVT = Op.getValueType();
  SDValue Addr = DAG.getTargetExternalSymbol(N->getSymbol(), PtrVT);
  return DAG.getNode(ScryISD::WRAPPER, DL, PtrVT, Addr);
}

SDValue ScryTargetLowering::LowerBlockAddress(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDLoc DL(Op);
  auto *N = cast<BlockAddressSDNode>(Op);
  EVT PtrVT = Op.getValueType();
  SDValue Addr = DAG.getTargetBlockAddress(N->getBlockAddress(), PtrVT,
                                           N->getOffset());
  return DAG.getNode(ScryISD::WRAPPER, DL, PtrVT, Addr);
}

SDValue ScryTargetLowering::LowerConstantPool(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDLoc DL(Op);
  auto *N = cast<ConstantPoolSDNode>(Op);
  EVT PtrVT = Op.getValueType();
  SDValue Addr = N->isMachineConstantPoolEntry()
                     ? DAG.getTargetConstantPool(N->getMachineCPVal(), PtrVT,
                                                 N->getAlign(), N->getOffset())
                     : DAG.getTargetConstantPool(N->getConstVal(), PtrVT,
                                                 N->getAlign(), N->getOffset());
  return DAG.getNode(ScryISD::WRAPPER, DL, PtrVT, Addr);
}

SDValue ScryTargetLowering::LowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  auto *FuncInfo = MF.getInfo<ScryFunctionInfo>();
  SDLoc DL(Op);
  EVT PtrVT = getPointerTy(MF.getDataLayout());
  // The va_list is the pointer to the caller's buffer of variadic arguments,
  // which arrived as a hidden trailing argument.
  SDValue Ptr = DAG.getCopyFromReg(Op.getOperand(0), DL,
                                   FuncInfo->getVarArgsBufferVreg(), PtrVT);
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Ptr.getValue(1), DL, Ptr, Op.getOperand(1),
                      MachinePointerInfo(SV));
}

//===----------------------------------------------------------------------===//
// Calling convention
//===----------------------------------------------------------------------===//

/// Running layout of the stack-passed argument area.
namespace {
struct StackArgLayout {
  unsigned Offset = 0;

  /// Reserve space for a value of the given size, returning its offset.
  unsigned add(unsigned Size) {
    unsigned Align = std::min(Size, 16u);
    Offset = alignTo(Offset, Align);
    unsigned Res = Offset;
    Offset += Size;
    return Res;
  }
};
} // namespace

/// Scalar integers narrower than 32 bits are passed as 32-bit values.
static EVT getPromotedArgVT(EVT VT) {
  if (VT.isInteger() && VT.bitsLT(MVT::i32))
    return MVT::i32;
  return VT;
}

SDValue ScryTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  auto *FuncInfo = MF.getInfo<ScryFunctionInfo>();
  EVT PtrVT = getPointerTy(MF.getDataLayout());

  unsigned NumOperandArgs = 0;
  StackArgLayout Layout;

  for (const ISD::InputArg &In : Ins) {
    if (In.Flags.isByVal()) {
      fail(DL, DAG, "Scry does not support byval arguments");
      InVals.push_back(DAG.getUNDEF(In.VT));
      continue;
    }
    // Integers narrower than 32 bits are passed as 32-bit values (see
    // LowerCall), so receive the wide value and truncate it.
    EVT ArgVT = getPromotedArgVT(In.VT);
    SDValue Val;
    if (NumOperandArgs < MaxTriggerOperands) {
      Val = DAG.getNode(ScryISD::ARG, DL, ArgVT,
                        DAG.getTargetConstant(NumOperandArgs, DL, MVT::i32));
      ++NumOperandArgs;
    } else {
      unsigned Size = ArgVT.getStoreSize();
      unsigned Offset = Layout.add(Size);
      int FI = MFI.CreateFixedObject(Size, Offset, /*IsImmutable=*/true);
      SDValue Addr = DAG.getFrameIndex(FI, PtrVT);
      Val = DAG.getLoad(ArgVT, DL, Chain, Addr,
                        MachinePointerInfo::getFixedStack(MF, FI));
    }
    if (ArgVT != In.VT)
      Val = DAG.getNode(ISD::TRUNCATE, DL, In.VT, Val);
    InVals.push_back(Val);
  }

  if (IsVarArg) {
    // The caller passes a pointer to a buffer holding the variadic arguments
    // as a hidden argument after the named ones.
    SDValue Ptr;
    if (NumOperandArgs < MaxTriggerOperands) {
      Ptr = DAG.getNode(ScryISD::ARG, DL, PtrVT,
                        DAG.getTargetConstant(NumOperandArgs, DL, MVT::i32));
      ++NumOperandArgs;
    } else {
      unsigned Offset = Layout.add(4);
      int FI = MFI.CreateFixedObject(4, Offset, /*IsImmutable=*/true);
      Ptr = DAG.getLoad(PtrVT, DL, Chain, DAG.getFrameIndex(FI, PtrVT),
                        MachinePointerInfo::getFixedStack(MF, FI));
    }
    Register VReg =
        MF.getRegInfo().createVirtualRegister(&Scry::GR32RegClass);
    Chain = DAG.getCopyToReg(Chain, DL, VReg, Ptr);
    FuncInfo->setVarArgsBufferVreg(VReg);
  }

  FuncInfo->setNumArgOperands(NumOperandArgs);
  FuncInfo->setIncomingArgsSize(alignTo(Layout.Offset, 16));
  return Chain;
}

SDValue ScryTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                      SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc &DL = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins = CLI.Ins;
  SDValue Chain = CLI.Chain;
  SDValue Callee = CLI.Callee;
  MachineFunction &MF = DAG.getMachineFunction();
  EVT PtrVT = getPointerTy(MF.getDataLayout());

  // Tail calls are never performed.
  CLI.IsTailCall = false;

  if (Ins.size() > 1) {
    fail(DL, DAG, "Scry supports at most one return value per call");
    return Chain;
  }

  // Classify arguments: operand-passed vs. stack-passed.  Variadic arguments
  // are written to a buffer in this function's frame whose address is passed
  // as a hidden trailing argument.
  SmallVector<SDValue, 4> OperandArgs;
  SmallVector<std::pair<SDValue, unsigned>, 8> StackArgs; // (value, offset)
  StackArgLayout Layout;
  SmallVector<std::pair<SDValue, unsigned>, 8> VarArgs; // (value, offset)
  StackArgLayout VarLayout;
  for (unsigned i = 0, e = Outs.size(); i != e; ++i) {
    const ISD::OutputArg &Out = Outs[i];
    if (Out.Flags.isByVal()) {
      fail(DL, DAG, "Scry does not support byval arguments");
      continue;
    }
    // Integers narrower than 32 bits are passed as 32-bit values, extended
    // according to the parameter's signedness.  Besides matching the C
    // integer promotions this keeps calls the DAG itself generates (such as
    // the memset libcall, whose value argument is an i8) compatible with the
    // C implementations of those functions.
    SDValue Val = OutVals[i];
    EVT ArgVT = getPromotedArgVT(Out.VT);
    if (ArgVT != Out.VT) {
      unsigned ExtOp = Out.Flags.isSExt()   ? ISD::SIGN_EXTEND
                       : Out.Flags.isZExt() ? ISD::ZERO_EXTEND
                                            : ISD::ANY_EXTEND;
      Val = DAG.getNode(ExtOp, DL, ArgVT, Val);
    }
    unsigned Size = ArgVT.getStoreSize();
    if (Out.Flags.isVarArg()) {
      VarArgs.push_back({Val, VarLayout.add(Size)});
      continue;
    }
    if (OperandArgs.size() < MaxTriggerOperands) {
      OperandArgs.push_back(Val);
      continue;
    }
    StackArgs.push_back({Val, Layout.add(Size)});
  }

  SmallVector<SDValue, 8> MemOpChains;
  if (CLI.IsVarArg) {
    MachineFrameInfo &MFI = MF.getFrameInfo();
    int FI = MFI.CreateStackObject(std::max(VarLayout.Offset, 1u), Align(16),
                                   /*isSpillSlot=*/false);
    SDValue Buf = DAG.getFrameIndex(FI, PtrVT);
    for (auto &VA : VarArgs) {
      SDValue Addr = DAG.getObjectPtrOffset(DL, Buf, TypeSize::getFixed(VA.second));
      MemOpChains.push_back(DAG.getStore(
          Chain, DL, VA.first, Addr,
          MachinePointerInfo::getFixedStack(MF, FI, VA.second)));
    }
    if (OperandArgs.size() < MaxTriggerOperands)
      OperandArgs.push_back(Buf);
    else
      StackArgs.push_back({Buf, Layout.add(4)});
  }
  if (!MemOpChains.empty()) {
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);
    MemOpChains.clear();
  }
  unsigned NumBytes = alignTo(Layout.Offset, 16);

  Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, DL);

  // Store stack-passed arguments into the outgoing area.
  for (auto &SA : StackArgs) {
    SDValue Addr = DAG.getNode(ScryISD::OUTADDR, DL, PtrVT,
                               DAG.getTargetConstant(SA.second, DL, MVT::i32));
    MemOpChains.push_back(
        DAG.getStore(Chain, DL, SA.first, Addr,
                     MachinePointerInfo::getStack(MF, SA.second)));
  }
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);

  // The callee stays an ordinary address value, even for a symbol: it is
  // then materialised by its own ADDR_SYM, which machine LICM can hoist out
  // of a loop and machine CSE can share between calls, and which the
  // lowering otherwise rebuilds right in front of the call.

  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  Ops.append(OperandArgs.begin(), OperandArgs.end());

  // Like arguments, integer results narrower than 32 bits travel as 32-bit
  // values (LowerReturn widens them); truncate to the type the caller
  // expects.  LLVM only widens zeroext/signext returns on the callee side,
  // so this is what keeps both sides of a call consistent.
  SmallVector<EVT, 2> ResultVTs;
  if (Ins.size() == 1)
    ResultVTs.push_back(getPromotedArgVT(Ins[0].VT));
  ResultVTs.push_back(MVT::Other);

  SDValue Call = DAG.getNode(ScryISD::CALL, DL, DAG.getVTList(ResultVTs), Ops);
  Chain = Call.getValue(ResultVTs.size() - 1);
  if (Ins.size() == 1) {
    SDValue Res = Call.getValue(0);
    if (ResultVTs[0] != Ins[0].VT)
      Res = DAG.getNode(ISD::TRUNCATE, DL, Ins[0].VT, Res);
    InVals.push_back(Res);
  }

  Chain = DAG.getCALLSEQ_END(Chain, NumBytes, 0, SDValue(), DL);
  return Chain;
}

bool ScryTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  return Outs.size() <= MaxTriggerOperands;
}

SDValue ScryTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  SmallVector<SDValue, 5> RetOps(1, Chain);
  for (unsigned i = 0, e = Outs.size(); i != e; ++i) {
    // Integer results narrower than 32 bits are returned as 32-bit values,
    // extended according to the function's return attributes.
    SDValue Val = OutVals[i];
    EVT RetVT = getPromotedArgVT(Outs[i].VT);
    if (RetVT != Outs[i].VT) {
      unsigned ExtOp = Outs[i].Flags.isSExt()   ? ISD::SIGN_EXTEND
                       : Outs[i].Flags.isZExt() ? ISD::ZERO_EXTEND
                                                : ISD::ANY_EXTEND;
      Val = DAG.getNode(ExtOp, DL, RetVT, Val);
    }
    RetOps.push_back(Val);
  }
  return DAG.getNode(ScryISD::RET, DL, MVT::Other, RetOps);
}
