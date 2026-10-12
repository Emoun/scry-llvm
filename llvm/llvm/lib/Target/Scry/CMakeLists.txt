add_llvm_component_group(Scry)

set(LLVM_TARGET_DEFINITIONS Scry.td)

tablegen(LLVM ScryGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM ScryGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM ScryGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM ScryGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM ScryGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM ScryGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(ScryCommonTableGen)

add_llvm_target(ScryCodeGen
  ScryAsmPrinter.cpp
  ScryFrameLowering.cpp
  ScryInstrInfo.cpp
  ScryISelDAGToDAG.cpp
  ScryISelLowering.cpp
  ScryMachineFunctionInfo.cpp
  ScryRegisterInfo.cpp
  ScrySlotAllocation.cpp
  ScrySubtarget.cpp
  ScryTargetMachine.cpp
  ScryTemporalLowering.cpp

  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  ScryDesc
  ScryInfo
  SelectionDAG
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT
  Scry
  )

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
