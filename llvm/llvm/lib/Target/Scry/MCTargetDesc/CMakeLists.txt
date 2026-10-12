add_llvm_component_library(LLVMScryDesc
  ScryAsmBackend.cpp
  ScryELFObjectWriter.cpp
  ScryInstPrinter.cpp
  ScryMCAsmInfo.cpp
  ScryMCCodeEmitter.cpp
  ScryMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  ScryInfo
  Support
  TargetParser

  ADD_TO_COMPONENT
  Scry
  )
