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
