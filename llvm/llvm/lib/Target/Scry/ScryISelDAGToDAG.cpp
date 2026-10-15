//===-- ScryISelDAGToDAG.cpp - A dag to dag inst selector for Scry --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the Scry target.
//
//===----------------------------------------------------------------------===//

#include "Scry.h"
#include "ScryISelLowering.h"
#include "ScryTargetMachine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "scry-isel"
#define PASS_NAME "Scry DAG->DAG Pattern Instruction Selection"

namespace {

class ScryDAGToDAGISel : public SelectionDAGISel {
public:
  ScryDAGToDAGISel() = delete;

  explicit ScryDAGToDAGISel(ScryTargetMachine &TM, CodeGenOptLevel OptLevel)
      : SelectionDAGISel(TM, OptLevel) {}

  void Select(SDNode *N) override;

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    InlineAsm::ConstraintCode ConstraintID,
                                    std::vector<SDValue> &OutOps) override {
    return true;
  }

#include "ScryGenDAGISel.inc"

  bool SelectAddrIdx(SDNode *Root, SDValue N, SDValue &Base, SDValue &Idx);

private:
  void selectFrameIndex(SDNode *N);
  void selectCall(SDNode *N);
};

class ScryDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  explicit ScryDAGToDAGISelLegacy(ScryTargetMachine &TM,
                                  CodeGenOptLevel OptLevel)
      : SelectionDAGISelLegacy(
            ID, std::make_unique<ScryDAGToDAGISel>(TM, OptLevel)) {}
};

} // end anonymous namespace

char ScryDAGToDAGISelLegacy::ID = 0;

INITIALIZE_PASS(ScryDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME, false, false)

void ScryDAGToDAGISel::selectFrameIndex(SDNode *N) {
  SDLoc DL(N);
  int FI = cast<FrameIndexSDNode>(N)->getIndex();
  SDValue TFI = CurDAG->getTargetFrameIndex(FI, MVT::i32);
  ReplaceNode(N, CurDAG->getMachineNode(Scry::ADDR_FI, DL, MVT::i32, TFI));
}

void ScryDAGToDAGISel::selectCall(SDNode *N) {
  // Operands: chain, callee, args...  Results: [value,] chain.
  SDLoc DL(N);
  SDValue Chain = N->getOperand(0);
  SDValue Callee = N->getOperand(1);
  bool Direct = Callee.getOpcode() == ISD::TargetGlobalAddress ||
                Callee.getOpcode() == ISD::TargetExternalSymbol;

  unsigned Opc;
  if (N->getNumValues() == 1) {
    Opc = Direct ? Scry::CALL_V : Scry::CALLI_V;
  } else {
    switch (N->getSimpleValueType(0).SimpleTy) {
    case MVT::i8:
      Opc = Direct ? Scry::CALL_8 : Scry::CALLI_8;
      break;
    case MVT::i16:
      Opc = Direct ? Scry::CALL_16 : Scry::CALLI_16;
      break;
    case MVT::i32:
      Opc = Direct ? Scry::CALL_32 : Scry::CALLI_32;
      break;
    case MVT::i64:
      Opc = Direct ? Scry::CALL_64 : Scry::CALLI_64;
      break;
    default:
      report_fatal_error("Scry: unsupported call result type");
    }
  }

  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Callee);
  for (unsigned i = 2, e = N->getNumOperands(); i != e; ++i)
    Ops.push_back(N->getOperand(i));
  Ops.push_back(Chain);

  SmallVector<EVT, 2> VTs;
  for (unsigned i = 0, e = N->getNumValues(); i != e; ++i)
    VTs.push_back(N->getValueType(i));

  MachineSDNode *Call = CurDAG->getMachineNode(Opc, DL, CurDAG->getVTList(VTs), Ops);
  ReplaceNode(N, Call);
}

/// [base, idx] addressing for the memory access Root: the simulator scales
/// an unsigned index by the access size, so `add nuw base, idx << log2(size)`
/// and `add nuw base, c` (c a non-negative multiple of the size) qualify.
bool ScryDAGToDAGISel::SelectAddrIdx(SDNode *Root, SDValue N, SDValue &Base,
                                     SDValue &Idx) {
  auto *Mem = dyn_cast<MemSDNode>(Root);
  if (!Mem || N.getOpcode() != ISD::ADD || !N->getFlags().hasNoUnsignedWrap())
    return false;
  unsigned Size = Mem->getMemoryVT().getStoreSize();
  if (!isPowerOf2_32(Size) || Size > 8)
    return false;
  unsigned K = Log2_32(Size);
  SDLoc DL(N);
  auto IsBaseLike = [](SDValue V) {
    return V.getOpcode() == ScryISD::WRAPPER ||
           V.getOpcode() == ISD::FrameIndex || V.getOpcode() == ISD::CopyFromReg;
  };
  for (unsigned Swap = 0; Swap != 2; ++Swap) {
    SDValue B = N.getOperand(Swap), Off = N.getOperand(1 - Swap);
    if (auto *C = dyn_cast<ConstantSDNode>(Off)) {
      int64_t V = C->getSExtValue();
      if (V < 0 || V % Size != 0)
        return false;
      Base = B;
      SDValue Imm = CurDAG->getTargetConstant(V / Size, DL, MVT::i32);
      Idx = SDValue(CurDAG->getMachineNode(Scry::CONST_32, DL, MVT::i32, Imm), 0);
      return true;
    }
    if (K == 0) {
      if (Swap == 0 && !IsBaseLike(B) && IsBaseLike(Off))
        continue;
      Base = B;
      Idx = Off;
      return true;
    }
    if (Off.getOpcode() == ISD::SHL && isa<ConstantSDNode>(Off.getOperand(1)) &&
        Off.getConstantOperandVal(1) == K) {
      Base = B;
      Idx = Off.getOperand(0);
      return true;
    }
  }
  return false;
}

void ScryDAGToDAGISel::Select(SDNode *N) {
  if (N->isMachineOpcode()) {
    N->setNodeId(-1);
    return;
  }

  switch (N->getOpcode()) {
  case ISD::FrameIndex:
    selectFrameIndex(N);
    return;
  case ScryISD::CALL:
    selectCall(N);
    return;
  default:
    break;
  }

  SelectCode(N);
}

FunctionPass *llvm::createScryISelDag(ScryTargetMachine &TM,
                                      CodeGenOptLevel OptLevel) {
  return new ScryDAGToDAGISelLegacy(TM, OptLevel);
}
