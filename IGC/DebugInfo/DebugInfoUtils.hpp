/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#pragma once

#include "llvm/Config/llvm-config.h"
#include "common/LLVMWarningsPush.hpp"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvmWrapper/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "common/LLVMWarningsPop.hpp"

#include "Version.hpp"

namespace IGC
{

    /// @brief DebugInfoUtils holds static helper functions for debug info support.
    class DebugInfoUtils
    {
    public:
        /// @brief return true if given module contain debug info
        /// @param M The LLVM module.
        /// @return true if given module contain debug info
        static bool HasDebugInfo(llvm::Module& M)
        {
            llvm::NamedMDNode* CU_Nodes = M.getNamedMetadata("llvm.dbg.cu");
            return (CU_Nodes != nullptr);
        }

        /// @brief creates a new call instruction to llvm.dbg.value intrinsic with
        ///        same information as in debug info of given global variable and
        ///        with value set to new given value.
        /// @param pGlobalVar global variable to handle its debug info
        /// @param pNewVal new value to map to the source variable (in the debug info)
        /// @param pEntryPoint entry point instruction to add new instructions before.
        /// @isIndirect true iff pNewValue type is a pointer to source variable type.
        /// @return new call instruction to llvm.dbg.value intrinsic
        static llvm::Instruction* UpdateGlobalVarDebugInfo(
            llvm::GlobalVariable* pGlobalVar, llvm::Value* pNewVal,
            llvm::Instruction* pEntryPoint, bool isIndirect)
        {
            llvm::Function* userFunc = pEntryPoint->getParent()->getParent();
            llvm::Module& M = *userFunc->getParent();
            llvm::NamedMDNode* CU_Nodes = M.getNamedMetadata("llvm.dbg.cu");
            if (!CU_Nodes)
            {
                return nullptr;
            }

            llvm::DINode::DIFlags flags = llvm::DINode::FlagZero;
            llvm::DIScope* spScope = nullptr;
            llvm::DILocation* loc = nullptr;
            bool done = false;
            for (auto bbIt = userFunc->begin();
                bbIt != userFunc->end() && !done;
                bbIt++)
            {
                for (auto instIt = bbIt->begin();
                    instIt != bbIt->end();
                    instIt++)
                {
                    // Discover first valid Loc in function
                    // and use it in dbg.declare nodes inserted
                    // later. Make sure the location belongs to
                    // the function and not to an inlined
                    // callee.
                    if (instIt->getDebugLoc() &&
                        !instIt->getDebugLoc().getInlinedAt())
                    {
                        loc = instIt->getDebugLoc().get();
                        spScope = loc->getScope()->getSubprogram();
                        done = true;
                        break;
                    }
                }
            }

            llvm::SmallVector<llvm::DIGlobalVariableExpression*, 1> GVs;
            pGlobalVar->getDebugInfo(GVs);
            for (unsigned int j = 0; j < GVs.size(); j++)
            {
                IGCLLVM::DIBuilder Builder(M);
                llvm::DIGlobalVariable* GV = GVs[j]->getVariable();
                llvm::DIScope* scopeToUse = GV->getScope();
                llvm::DILocation* locToUse = llvm::DILocation::get(scopeToUse->getContext(), GV->getLine(), 0, scopeToUse, loc);
                if (llvm::isa<llvm::DICompileUnit>(GV->getScope()))
                {
                    // Function has no DebugLoc so it is either internal
                    // or optimized. So there is no point inserting
                    // global var metadata as "local" to function.
                    if (!done)
                        continue;

                    // Use scope of current sub-program
                    scopeToUse = spScope;
                    locToUse = loc;
                }
                llvm::DIVariable* Var = Builder.createAutoVariable(scopeToUse,
                    GV->getDisplayName(),
                    Builder.createFile(GV->getFilename(), GV->getDirectory()),
                    GV->getLine(),
                    GV->getType(),
                    false,
                    flags
                );

                if (isIndirect)
                    return Builder.insertDeclare(pNewVal, llvm::cast<llvm::DILocalVariable>(Var), Builder.createExpression(), locToUse, pEntryPoint);

                return Builder.insertDbgValueIntrinsic(pNewVal, 0, llvm::cast<llvm::DILocalVariable>(Var), Builder.createExpression(), locToUse, pEntryPoint);
            }
            return nullptr;
        }

        static bool IsSpecialDebugVariable(const std::string& name)
        {
            // Check for OCL special debug variable such as:
            //   "__ocl_dbg_gid0"
            //   "__ocl_dbg_gid1"
            //   "__ocl_dbg_gid2"
            // Assuming all OCL debug special variables starts with "__ocl_dbg" prefix.
            return (name.find("__ocl_dbg", 0) == 0);
        }

    private:
        /// @brief Private deafault constructor to prevent creating instance of DebugInfoUtils
        DebugInfoUtils();
    };

} // namespace IGC
