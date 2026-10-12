add_llvm_component_library(LLVMScryInfo
  ScryTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  Scry
  )
