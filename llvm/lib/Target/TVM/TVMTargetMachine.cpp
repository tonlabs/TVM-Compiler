//===-- TVMTargetMachine.cpp - Define TargetMachine for TVM ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the TVM specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#include "TVMTargetMachine.h"
#include "TVM.h"

#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetRegistry.h"

namespace llvm {

extern "C" void LLVMInitializeTVMTarget() {
  RegisterTargetMachine<TVMTargetMachine> X(getTheTVMTarget());
  auto &PR = *PassRegistry::getPassRegistry();
  initializeTVMArgumentMovePass(PR);
  initializeTVMReplacePhysRegsPass(PR);
  initializeTVMRegStackifyPass(PR);
  initializeTVMRegNumberingPass(PR);
  initializeTVMExplicitLocalsPass(PR);
}

static Reloc::Model getEffectiveRelocModel(Optional<Reloc::Model> RM) {
  if (!RM.hasValue())
    return Reloc::Static;
  return *RM;
}

static CodeModel::Model getEffectiveCodeModel(Optional<CodeModel::Model> CM) {
  if (CM)
    return *CM;
  return CodeModel::Small;
}

static std::string computeDataLayout(const Triple &TT, StringRef CPU,
                                     const TargetOptions &Options) {
  return "E-S1024-i256:256:256";
}

TVMTargetMachine::TVMTargetMachine(const Target &T, const Triple &TT,
                                   StringRef CPU, StringRef FS,
                                   const TargetOptions &Options,
                                   Optional<Reloc::Model> RM,
                                   Optional<CodeModel::Model> CM,
                                   CodeGenOpt::Level OL, bool JIT)
    : LLVMTargetMachine(T, computeDataLayout(TT, CPU, Options), TT, CPU, FS,
                        Options, getEffectiveRelocModel(RM),
                        getEffectiveCodeModel(CM), OL),
      TLOF(make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT, CPU, FS, *this) {
  initAsmInfo();
}

namespace {
/// TVM Code Generator Pass Configuration Options.
class TVMPassConfig : public TargetPassConfig {
public:
  TVMPassConfig(TVMTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  TVMTargetMachine &getTVMTargetMachine() const {
    return getTM<TVMTargetMachine>();
  }

  FunctionPass *createTargetRegisterAllocator(bool) override;

  bool addInstSelector() override;
  void addPreEmitPass() override;
  void addPostRegAlloc() override;
};
} // namespace

FunctionPass *TVMPassConfig::createTargetRegisterAllocator(bool) {
  return nullptr; // No reg alloc
}

TargetPassConfig *TVMTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new TVMPassConfig(*this, PM);
}

bool TVMPassConfig::addInstSelector() {
  (void)TargetPassConfig::addInstSelector();
  // Install an instruction selector.
  addPass(createTVMISelDag(getTVMTargetMachine(), getOptLevel()));
  // Run the argument-move pass immediately after the ScheduleDAG scheduler
  // so that we can fix up the ARGUMENT instructions before anything else
  // sees them in the wrong place.
  addPass(createTVMArgumentMove());
  return false;
}

void TVMPassConfig::addPreEmitPass() {
  TargetPassConfig::addPreEmitPass();

  // Now that we have a prologue and epilogue and all frame indices are
  // rewritten, eliminate SP and FP. This allows them to be stackified,
  // colored, and numbered with the rest of the registers.
  addPass(createTVMReplacePhysRegs());

  addPass(createTVMRegStackify());
  addPass(createTVMExplicitLocals());

  // Create a mapping from LLVM CodeGen virtual registers to wasm registers.
  addPass(createTVMRegNumbering());
}

void TVMPassConfig::addPostRegAlloc() {
  // TODO: The following CodeGen passes don't currently support code containing
  // virtual registers. Consider removing their restrictions and re-enabling
  // them.

  // These functions all require the NoVRegs property.
  disablePass(&MachineCopyPropagationID);
  disablePass(&PostRAMachineSinkingID);
  disablePass(&PostRASchedulerID);
  disablePass(&FuncletLayoutID);
  disablePass(&StackMapLivenessID);
  disablePass(&LiveDebugValuesID);
  disablePass(&PatchableFunctionID);
  disablePass(&ShrinkWrapID);

  TargetPassConfig::addPostRegAlloc();
}

} // end of namespace llvm
