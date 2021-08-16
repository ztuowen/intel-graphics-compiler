/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

//
// This file implements the GenX specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "IGC/common/StringMacros.hpp"

#include "GenXSubtarget.h"
#include "common/StringMacros.hpp"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "Probe/Assertion.h"

using namespace llvm;

#define DEBUG_TYPE "subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#define GET_SUBTARGETINFO_MC_DESC
#include "GenXGenSubtargetInfo.inc"

static cl::opt<bool>
    StackScratchMem("stack-scratch-mem",
                    cl::desc("Specify what surface should be used for stack"),
                    cl::init(true));

void GenXSubtarget::resetSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS) {

  DumpRegAlloc = false;
  EmitCisa = false;
  HasLongLong = false;
  HasFP64 = false;
  DisableJmpi = false;
  DisableVectorDecomposition = false;
  DisableJumpTables = false;
  WarnCallable = false;
  EmulateLongLong = false;
  HasAdd64 = false;
  UseMulDDQ = false;
  OCLRuntime = false;
  HasSwitchjmp = false;
  WaNoMaskFusedEU = false;
  HasIntDivRem32 = false;

  if (StackScratchMem)
    StackSurf = PreDefined_Surface::PREDEFINED_SURFACE_T255;
  else
    StackSurf = PreDefined_Surface::PREDEFINED_SURFACE_STACK;

  GenXVariant = llvm::StringSwitch<GenXTag>(CPU)
    .Case("HSW", GENX_HSW)
    .Case("BDW", GENX_BDW)
    .Case("CHV", GENX_CHV)
    .Case("SKL", GENX_SKL)
    .Case("BXT", GENX_BXT)
    .Case("KBL", GENX_KBL)
    .Case("GLK", GENX_GLK)
    .Case("CNL", GENX_CNL)
    .Case("ICLLP", GENX_ICLLP)
    .Case("TGLLP", GENX_TGLLP)
    .Case("DG1", GENX_DG1)
    .Case("XEHP", XE_HP_SDV)
    .Default(GENX_SKL);

  std::string CPUName(CPU);
  if (CPUName.empty())
    CPUName = "generic";

  ParseSubtargetFeatures(CPUName, TuneCPU, FS);
}

GenXSubtarget::GenXSubtarget(const Triple &TT, const std::string &CPU,
                             const std::string &TC, const std::string &FS)
    : GenXGenSubtargetInfo(TT, CPU, TC, FS), TargetTriple(TT) {

  resetSubtargetFeatures(CPU, TC, FS);
}

