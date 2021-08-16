/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

//
// This file defines the GenX specific subclass of TargetMachine.
//
/// Non-pass classes
/// ================
///
/// This section documents some GenX backend classes and abstractions that are not
/// in themselves passes, but are used by the passes.
///
/// .. include:: GenXAlignmentInfo.h
///
/// .. include:: GenXRegion.h
///
/// .. include:: GenXSubtarget.h
///
/// Pass documentation
/// ==================
///
/// The GenX backend runs the following passes on LLVM IR:
///
/// .. contents::
///    :local:
///    :depth: 1
///
//
//===----------------------------------------------------------------------===//

#include "GenXTargetMachine.h"

#include "FunctionGroup.h"
#include "GenX.h"
#include "GenXDebugInfo.h"
#include "GenXModule.h"

#include "vc/GenXCodeGen/GenXOCLRuntimeInfo.h"
#include "vc/GenXOpts/GenXOpts.h"
#include "vc/Support/BackendConfig.h"

#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

using namespace llvm;

static cl::opt<bool> ExperimentalEnforceLateEmulationImports(
    "vc-experimental-emulation-late-imports", cl::init(false), cl::Hidden,
    cl::desc("Import of some emulation BiF shall be deferred (experimental)"));

static cl::opt<bool> EmitVLoadStore(
    "genx-emit-vldst", cl::init(true), cl::Hidden,
    cl::desc("Emit load/store intrinsic calls for pass-by-ref arguments"));

// There's another copy of DL string in clang/lib/Basic/Targets.cpp
static std::string getDL(bool Is64Bit) {
  return Is64Bit ? "e-p:64:64-i64:64-n8:16:32:64" : "e-p:32:32-i64:64-n8:16:32";
}

namespace llvm {

//===----------------------------------------------------------------------===//
// This function is required to add GenX passes to opt tool
//===----------------------------------------------------------------------===//
void initializeGenXPasses(PassRegistry &registry) {
  initializeFunctionGroupAnalysisPass(registry);
  initializeGenXAddressCommoningPass(registry);
  initializeGenXArgIndirectionPass(registry);
  initializeGenXCategoryPass(registry);
  initializeGenXCFSimplificationPass(registry);
  initializeGenXCisaBuilderPass(registry);
  initializeGenXCoalescingPass(registry);
  initializeGenXDeadVectorRemovalPass(registry);
  initializeGenXDepressurizerPass(registry);
  initializeGenXEarlySimdCFConformancePass(registry);
  initializeGenXEmulationImportPass(registry);
  initializeGenXEmulatePass(registry);
  initializeGenXExtractVectorizerPass(registry);
  initializeGenXVectorCombinerPass(registry);
  initializeGenXFuncBalingPass(registry);
  initializeGenXGEPLoweringPass(registry);
  initializeGenXGroupBalingPass(registry);
  initializeGenXIMadPostLegalizationPass(registry);
  initializeGenXLateSimdCFConformancePass(registry);
  initializeGenXLayoutBlocksPass(registry);
  initializeGenXLegalizationPass(registry);
  initializeGenXLiveRangesPass(registry);
  initializeGenXLivenessPass(registry);
  initializeGenXLivenessPass(registry);
  initializeGenXLowerAggrCopiesPass(registry);
  initializeGenXLoweringPass(registry);
  initializeGenXModulePass(registry);
  initializeGenXNumberingPass(registry);
  initializeGenXPatternMatchPass(registry);
  initializeGenXPostLegalizationPass(registry);
  initializeGenXPrologEpilogInsertionPass(registry);
  initializeGenXPromotePredicatePass(registry);
  initializeGenXRawSendRipperPass(registry);
  initializeGenXReduceIntSizePass(registry);
  initializeGenXRegionCollapsingPass(registry);
  initializeGenXRematerializationPass(registry);
  initializeGenXThreadPrivateMemoryPass(registry);
  initializeGenXTidyControlFlowPass(registry);
  initializeGenXUnbalingPass(registry);
  initializeGenXVisaRegAllocPass(registry);
  initializeTransformPrivMemPass(registry);
  initializeGenXFunctionPointersLoweringPass(registry);
  initializeGenXBackendConfigPass(registry);
  initializeGenXImportOCLBiFPass(registry);
  initializeGenXSimplifyPass(registry);
  initializeCMABIPass(registry);
  initializeGenXLowerJmpTableSwitchPass(registry);
  initializeGenXGlobalValueLoweringPass(registry);
  initializeCMImpParamPass(registry);
  initializeCMKernelArgOffsetPass(registry);
  initializeGenXPrintfResolutionPass(registry);
  initializeGenXPrintfLegalizationPass(registry);
  initializeGenXAggregatePseudoLoweringPass(registry);
  initializeGenXBTIAssignmentPass(registry);

  // WRITE HERE MORE PASSES IF IT'S NEEDED;
}

TargetTransformInfo GenXTargetMachine::getTargetTransformInfo(const Function &F) {
  GenXTTIImpl GTTI(F.getParent()->getDataLayout());
  return TargetTransformInfo(GTTI);
}

} // namespace llvm

namespace {

class GenXPassConfig : public TargetPassConfig {
public:
  GenXPassConfig(GenXTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {
    // Cannot add INITIALIZE_PASS with needed dependencies because ID
    // is in parent TargetPassConfig class with its own initialization
    // routine.
    initializeGenXBackendConfigPass(*PassRegistry::getPassRegistry());
  }

  GenXTargetMachine &getGenXTargetMachine() const {
    return getTM<GenXTargetMachine>();
  }

  // PassConfig will always be available: in BE it is created inside
  // addPassesToEmitFile, opt creates it manually before adding other
  // passes. BackendConfig will be either created manually with
  // options structure or default-constructed using cl::opt values.
  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<GenXBackendConfig>();
    TargetPassConfig::getAnalysisUsage(AU);
  }

  // Should only be used after GenXPassConfig is added to PassManager.
  // Otherwise getAnalysis won't work.
  const GenXBackendConfig &getBackendConfig() const {
    return getAnalysis<GenXBackendConfig>();
  }
};

} // namespace

GenXTargetMachine::GenXTargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     const TargetOptions &Options,
                                     Optional<Reloc::Model> RM,
                                     Optional<CodeModel::Model> CM,
                                     CodeGenOpt::Level OL, bool Is64Bit)
    : IGCLLVM::LLVMTargetMachine(T, getDL(Is64Bit), TT, CPU, FS, Options,
                                 RM ? RM.getValue() : Reloc::Model::Static,
                                 CM ? CM.getValue() : CodeModel::Model::Small,
                                 OL),
      Is64Bit(Is64Bit), Subtarget(TT, CPU.str(), CPU.str(), FS.str()) {}

GenXTargetMachine::~GenXTargetMachine() = default;

static GenXPassConfig *createGenXPassConfig(GenXTargetMachine &TM,
                                            PassManagerBase &PM) {
  return new GenXPassConfig(TM, PM);
}

TargetPassConfig *GenXTargetMachine::createPassConfig(PassManagerBase &PM) {
  return createGenXPassConfig(*this, PM);
}

void GenXTargetMachine32::anchor() {}

GenXTargetMachine32::GenXTargetMachine32(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         Optional<Reloc::Model> RM,
                                         Optional<CodeModel::Model> CM,
                                         CodeGenOpt::Level OL, bool JIT)
    : GenXTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL, false) {}

void GenXTargetMachine64::anchor() {}

GenXTargetMachine64::GenXTargetMachine64(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         Optional<Reloc::Model> RM,
                                         Optional<CodeModel::Model> CM,
                                         CodeGenOpt::Level OL, bool JIT)
    : GenXTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL, true) {}

//===----------------------------------------------------------------------===//
//                       External Interface declaration
//===----------------------------------------------------------------------===//
extern "C" void LLVMInitializeGenXTarget() {
  // Register the target.
  RegisterTargetMachine<GenXTargetMachine32> X(getTheGenXTarget32());
  RegisterTargetMachine<GenXTargetMachine64> Y(getTheGenXTarget64());
}

extern "C" void LLVMInitializeGenXPasses() {
  llvm::initializeGenXPasses(*PassRegistry::getPassRegistry());
}

//===----------------------------------------------------------------------===//
// Pass Pipeline Configuration
//===----------------------------------------------------------------------===//
bool GenXTargetMachine::addPassesToEmitFile(PassManagerBase &PM,
                                            raw_pwrite_stream &o,
                                            raw_pwrite_stream * pi,
                                            CodeGenFileType FileType,
                                            bool DisableVerify,
                                            MachineModuleInfo *) {
  // We can consider the .isa file to be an object file, or an assembly file
  // which may later be converted to GenX code by the Finalizer. If we're
  // asked to produce any other type of file return true to indicate an error.
  if ((FileType != IGCLLVM::TargetMachine::CodeGenFileType::CGFT_ObjectFile) &&
      (FileType != IGCLLVM::TargetMachine::CodeGenFileType::CGFT_AssemblyFile))
    return true;

  GenXPassConfig *PassConfig = createGenXPassConfig(*this, PM);
  PM.add(PassConfig);
  const GenXBackendConfig &BackendConfig = PassConfig->getBackendConfig();

  // Install GenX-specific TargetTransformInfo for passes such as
  // LowerAggrCopies and InfoAddressSpace
  PM.add(createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  PM.add(createSROAPass());
  PM.add(createEarlyCSEPass());
  PM.add(createLowerExpectIntrinsicPass());
  PM.add(createCFGSimplificationPass());
  PM.add(createInstructionCombiningPass());

  PM.add(createGlobalDCEPass());
  PM.add(createGenXLowerAggrCopiesPass());
  PM.add(createInferAddressSpacesPass());
  PM.add(createTransformPrivMemPass());
  PM.add(createPromoteMemoryToRegisterPass());
    // All passes which modify the LLVM IR are now complete; run the verifier
  // to ensure that the IR is valid.
  if (!DisableVerify)
    PM.add(createVerifierPass());
  // Run passes to generate vISA.

  /// .. include:: GenXGEPLowering.cpp
  PM.add(createGenXGEPLoweringPass());
  PM.add(createGenXThreadPrivateMemoryPass());

  /// BasicAliasAnalysis
  /// ------------------
  /// This is a standard LLVM analysis pass to provide basic AliasAnalysis
  /// support.
  PM.add(createBasicAAWrapperPass());
  /// SROA
  /// ----
  /// This is a standard LLVM pass, used at this point in the GenX backend.
  /// Normally all alloca variables have been
  /// removed by now by earlier LLVM passes, unless ``-O0`` was specified.
  /// We run this pass here to cover that case.
  ///
  /// **IR restriction**: alloca, load, store not supported after this pass.
  ///
  PM.add(createSROAPass());

  PM.add(createGenXInstCombineCleanup());

  if (!ExperimentalEnforceLateEmulationImports)
    PM.add(createGenXEmulationImportPass());

  PM.add(createGenXLowerJmpTableSwitchPass());
  /// LowerSwitch
  /// -----------
  /// This is a standard LLVM pass to lower a switch instruction to a chain of
  /// conditional branches.
  ///
  /// **IR restriction**: switch not supported after this pass.
  ///
  // TODO: keep some switch instructions and lower them to JMPSWITCH vISA ops.
  PM.add(createLowerSwitchPass());
  /// .. include:: GenXCFSimplification.cpp
  PM.add(createGenXCFSimplificationPass());
  /// CFGSimplification
  /// -----------------
  /// This is a standard LLVM pass, used at this point in the GenX backend.
  ///
  PM.add(createCFGSimplificationPass());
  /// .. include:: GenXInlineAsmLowering.cpp
  PM.add(createGenXInlineAsmLoweringPass());
  /// .. include:: GenXReduceIntSize.cpp
  PM.add(createGenXReduceIntSizePass());
  /// .. include:: GenXGlobalValueLowering.cpp
  PM.add(createGenXGlobalValueLoweringPass());
  /// .. include:: GenXAggregatePseudoLowering.cpp
  PM.add(createGenXAggregatePseudoLoweringPass());
  /// InstructionCombining
  /// --------------------
  /// This is a standard LLVM pass, used at this point in the GenX backend.
  ///
  PM.add(createInstructionCombiningPass());
  // Run integer reduction again to revert some trunc/ext patterns transformed
  // by instcombine.
  PM.add(createGenXReduceIntSizePass());
  /// .. include:: GenXSimdCFConformance.cpp
  PM.add(createGenXEarlySimdCFConformancePass());
  /// .. include:: GenXPromotePredicate.cpp
  PM.add(createGenXPromotePredicatePass());
  // Run GEP lowering again to remove possible GEPs after instcombine.
  PM.add(createGenXGEPLoweringPass());
  /// .. include:: GenXLowering.cpp
  PM.add(createGenXLoweringPass());
  if (!DisableVerify) PM.add(createVerifierPass());
  PM.add(createGenXFunctionPointersLoweringPass());
  /// .. include:: GenXRegionCollapsing.cpp
  PM.add(createGenXRegionCollapsingPass());
  /// EarlyCSE
  /// --------
  /// This is a standard LLVM pass, run at this point in the GenX backend.
  /// It commons up common subexpressions, but only in the case that two common
  /// subexpressions are related by one dominating the other.
  ///
  PM.add(createEarlyCSEPass());
  /// BreakCriticalEdges
  /// ------------------
  /// In the control flow graph, a critical edge is one from a basic block with
  /// multiple successors (a conditional branch) to a basic block with multiple
  /// predecessors.
  ///
  /// We use this standard LLVM pass to split such edges, to ensure that
  /// constant loader and GenXCoalescing have somewhere to insert a phi copy if
  /// needed.
  ///
  PM.add(createBreakCriticalEdgesPass());
  /// .. include:: GenXPatternMatch.cpp
  PM.add(createGenXPatternMatchPass(&Options));
  if (!DisableVerify) PM.add(createVerifierPass());
  /// .. include:: GenXExtractVectorizer.cpp
  PM.add(createGenXExtractVectorizerPass());
  /// .. include:: GenXVectorCombiner.cpp
  PM.add(createGenXVectorCombinerPass());
  /// .. include:: GenXRawSendRipper.cpp
  PM.add(createGenXRawSendRipperPass());
  /// DeadCodeElimination
  /// -------------------
  /// This is a standard LLVM pass, run at this point in the GenX backend. It
  /// removes code that has been made dead by other passes.
  ///
  PM.add(createDeadCodeEliminationPass());
  PM.add(createGenXPrologEpilogInsertionPass());
  /// .. include:: GenXBaling.h
  PM.add(createGenXFuncBalingPass(BalingKind::BK_Legalization, &Subtarget));
  /// .. include:: GenXLegalization.cpp
  PM.add(createGenXLegalizationPass());
  if (ExperimentalEnforceLateEmulationImports)
    PM.add(createGenXEmulationImportPass());
  /// .. include:: GenXEmulate.cpp
  PM.add(createGenXEmulatePass());
  /// .. include:: GenXDeadVectorRemoval.cpp
  PM.add(createGenXDeadVectorRemovalPass());
  /// DeadCodeElimination
  /// -------------------
  /// This is a standard LLVM pass, run at this point in the GenX backend. It
  /// removes code that has been made dead by other passes.
  ///
  PM.add(createDeadCodeEliminationPass());
  /// .. include:: GenXPostLegalization.cpp
  /// .. include:: GenXConstants.cpp
  /// .. include:: GenXVectorDecomposer.h
  PM.add(createGenXPostLegalizationPass());
  if (!DisableVerify) PM.add(createVerifierPass());
  /// EarlyCSE
  /// --------
  /// This is a standard LLVM pass, run at this point in the GenX backend.
  /// It commons up common subexpressions, but only in the case that two common
  /// subexpressions are related by one dominating the other.
  ///
  PM.add(createEarlyCSEPass());
  /// LICM
  /// ----
  /// This is a standard LLVM pass to hoist/sink the loop invariant code after
  /// legalization.
  PM.add(createLICMPass());
  /// DeadCodeElimination
  /// -------------------
  /// This is a standard LLVM pass, run at this point in the GenX backend. It
  /// removes code that has been made dead by other passes.
  ///
  PM.add(createDeadCodeEliminationPass());
  PM.add(createGenXIMadPostLegalizationPass());
  /// GlobalDCE
  /// ---------
  /// This is a standard LLVM pass, run at this point in the GenX backend. It
  /// eliminates unreachable internal globals.
  ///
  PM.add(createGlobalDCEPass());
  /// .. include:: GenXModule.h
  PM.add(createGenXModulePass());
  /// .. include:: GenXLiveness.h
  PM.add(createGenXLivenessPass());
  PM.add(createGenXGroupBalingPass(BalingKind::BK_Analysis, &Subtarget));
  PM.add(createGenXNumberingPass());
  PM.add(createGenXLiveRangesPass());
  /// .. include:: GenXRematerialization.cpp
  PM.add(createGenXRematerializationPass());
  /// .. include:: GenXCategory.cpp
  PM.add(createGenXCategoryPass());
  /// Late SIMD CF conformance pass
  /// -----------------------------
  /// This is the same pass as GenXSimdCFConformance above, but run in a
  /// slightly different way. See above.
  ///
  /// **IR restriction**: After this pass, the EM values must have EM register
  /// category. The RM values must have RM register category. The !any result of
  /// a goto/join must have NONE register category.
  ///
  PM.add(createGenXLateSimdCFConformancePass());
  /// CodeGen baling pass
  /// -------------------
  /// This is the same pass as GenXBaling above, but run in a slightly different
  /// way. See above.
  ///
  /// **IR restriction**: Any pass after this needs to be careful when modifying
  /// code, as it also needs to update baling info.
  ///
  PM.add(createGenXGroupBalingPass(BalingKind::BK_CodeGen, &Subtarget));

  /// .. include:: GenXNumbering.h
  PM.add(createGenXNumberingPass());
  /// .. include:: GenXLiveRanges.cpp
  PM.add(createGenXLiveRangesPass());
  /// .. include:: GenXUnbaling.cpp
  PM.add(createGenXUnbalingPass());
  /// .. include:: GenXDepressurizer.cpp
  PM.add(createGenXDepressurizerPass());
  /// .. include:: GenXNumbering.h
  PM.add(createGenXNumberingPass());
  /// .. include:: GenXLiveRanges.cpp
  PM.add(createGenXLiveRangesPass());
  /// .. include:: GenXCoalescing.cpp
  PM.add(createGenXCoalescingPass());
  /// .. include:: GenXAddressCommoning.cpp
  PM.add(createGenXAddressCommoningPass());
  /// .. include:: GenXArgIndirection.cpp
  PM.add(createGenXArgIndirectionPass());
  /// .. include:: GenXTidyControlFlow.cpp
  PM.add(createGenXTidyControlFlowPass());
  /// .. include:: GenXVisaRegAlloc.h
  auto RegAlloc = createGenXVisaRegAllocPass();
  PM.add(RegAlloc);
  if (BackendConfig.enableRegAllocDump() || Subtarget.dumpRegAlloc())
    PM.add(createGenXGroupAnalysisDumperPass(RegAlloc, ".regalloc"));

  /// .. include:: GenXCisaBuilder.cpp
  PM.add(createGenXCisaBuilderPass());
  PM.add(createGenXFinalizerPass(o));
  PM.add(createGenXDebugInfoPass());

  // Analysis for collecting information related to OCL runtime. Can
  // be used by external caller by adding extractor pass in the end of
  // compilation pipeline.
  // Explicit construction can be omitted because adding of extractor
  // pass will create runtime info analysis. Leaving it exlicit for
  // clarity.
  if (Subtarget.isOCLRuntime())
    PM.add(new GenXOCLRuntimeInfo());

  return false;
}

void GenXTargetMachine::adjustPassManager(PassManagerBuilder &PMBuilder) {
  // Lower aggr copies.
  PMBuilder.addExtension(
      PassManagerBuilder::EP_EarlyAsPossible,
      [](const PassManagerBuilder &Builder, PassManagerBase &PM) {
        PM.add(createGenXLowerAggrCopiesPass());
      });

  // Packetize.
  auto AddPacketize = [](const PassManagerBuilder &Builder,
                         PassManagerBase &PM) {
    PM.add(createGenXPrintfResolutionPass());
    PM.add(createGenXImportOCLBiFPass());
    PM.add(createGenXPacketizePass());
    PM.add(createAlwaysInlinerLegacyPass());
    PM.add(createGenXPrintfLegalizationPass());
    PM.add(createGlobalDCEPass());
    PM.add(createPromoteMemoryToRegisterPass());
    PM.add(createInferAddressSpacesPass());
    PM.add(createEarlyCSEPass(true));
    PM.add(createCFGSimplificationPass());
    PM.add(createInstructionCombiningPass());
    PM.add(createDeadCodeEliminationPass());
    PM.add(createSROAPass());
    PM.add(createInferAddressSpacesPass());
    PM.add(createEarlyCSEPass(true));
    PM.add(createCFGSimplificationPass());
    PM.add(createInstructionCombiningPass());
    PM.add(createDeadCodeEliminationPass());
  };
  PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                         AddPacketize);
  PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                         AddPacketize);

  // vldst.
  if (EmitVLoadStore) {
    auto AddLowerLoadStore = [](const PassManagerBuilder &Builder,
                                PassManagerBase &PM) {
      if (Builder.OptLevel > 0) {
        // Inline
        PM.add(createSROAPass());
        PM.add(createEarlyCSEPass());
        PM.add(createJumpThreadingPass());
        PM.add(createCFGSimplificationPass());
        PM.add(createCorrelatedValuePropagationPass());
        PM.add(createGenXReduceIntSizePass());
        PM.add(createInstructionCombiningPass());
        PM.add(createAlwaysInlinerLegacyPass());
        PM.add(createGlobalDCEPass());
        PM.add(createInstructionCombiningPass());
        // Unroll
        PM.add(createCFGSimplificationPass());
        PM.add(createReassociatePass());
        PM.add(createLoopRotatePass());
        PM.add(createLICMPass());
        PM.add(createInstructionCombiningPass());
        PM.add(createIndVarSimplifyPass());
        PM.add(createLoopIdiomPass());
        PM.add(createLoopDeletionPass());
        PM.add(createSimpleLoopUnrollPass());
        PM.add(createInstructionCombiningPass());
        // Simplify region accesses.
        PM.add(createGenXRegionCollapsingPass());
        PM.add(createEarlyCSEPass());
        PM.add(createDeadCodeEliminationPass());
      }
      PM.add(createCMLowerVLoadVStorePass());
    };
    PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                           AddLowerLoadStore);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           AddLowerLoadStore);
  }

  // CM implicit parameters.
  auto AddCMImpParam = [this](const PassManagerBuilder &Builder,
                              PassManagerBase &PM) {
    PM.add(createCMImpParamPass(!Subtarget.isOCLRuntime()));
  };
  PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                         AddCMImpParam);
  PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                         AddCMImpParam);

  // CM ABI.
  auto AddCMABI = [](const PassManagerBuilder &Builder, PassManagerBase &PM) {
    PM.add(createIPSCCPPass());
    PM.add(createCMABIPass());
  };
  PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly, AddCMABI);
  PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0, AddCMABI);

  // BTI assignment.
  if (Subtarget.isOCLRuntime()) {
    auto AddBTIAssign = [](const PassManagerBuilder &Builder,
                           PassManagerBase &PM) {
      PM.add(createGenXBTIAssignmentPass());
    };
    PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                           AddBTIAssign);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           AddBTIAssign);
  }

  // CM kernel argument offset.
  auto AddCMKernelArgOffset = [this](const PassManagerBuilder &Builder,
                                     PassManagerBase &PM) {
    unsigned Width = 32;
    PM.add(createCMKernelArgOffsetPass(Width, Subtarget.isOCLRuntime()));
  };
  PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                         AddCMKernelArgOffset);
  PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                         AddCMKernelArgOffset);

  auto AddGenXPeephole = [](const PassManagerBuilder &Builder,
                            PassManagerBase &PM) {
    PM.add(createGenXSimplifyPass());
  };
  PMBuilder.addExtension(PassManagerBuilder::EP_Peephole, AddGenXPeephole);
}
