/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "Compiler/Optimizer/OpenCLPasses/WIFuncs/WIFuncResolution.hpp"
#include "Compiler/Optimizer/OpenCLPasses/WIFuncs/WIFuncsAnalysis.hpp"
#include "Compiler/IGCPassSupport.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/DerivedTypes.h>
#include "common/LLVMWarningsPop.hpp"
#include "Probe/Assertion.h"
#include <llvmWrapper/Support/Alignment.h>
#include <llvmWrapper/IR/DerivedTypes.h>

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-wi-func-resolution"
#define PASS_DESCRIPTION "Resolves work item functions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(WIFuncResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(WIFuncResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char WIFuncResolution::ID = 0;

WIFuncResolution::WIFuncResolution() : FunctionPass(ID), m_implicitArgs()
{
    initializeWIFuncResolutionPass(*PassRegistry::getPassRegistry());
}

Constant* WIFuncResolution::getKnownWorkGroupSize(
    IGCMD::MetaDataUtils* MDUtils, llvm::Function& F) const
{
    auto Dims = IGCMD::IGCMetaDataHelper::getThreadGroupDims(*MDUtils, &F);
    if (!Dims)
        return nullptr;

    return ConstantDataVector::get(F.getContext(), *Dims);
}

bool WIFuncResolution::runOnFunction(Function& F)
{
    m_changed = false;
    auto* MDUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    m_implicitArgs = ImplicitArgs(F, getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils());
    visit(F);

    /// If the work group size is known at compile time, emit it as a
    /// literal rather than reading from the payload.
    if (Constant * KnownWorkGroupSize = getKnownWorkGroupSize(MDUtils, F))
    {
        if (auto * Arg = m_implicitArgs.getImplicitArg(F, ImplicitArg::ENQUEUED_LOCAL_WORK_SIZE))
            Arg->replaceAllUsesWith(KnownWorkGroupSize);
    }

    return m_changed;
}

void WIFuncResolution::visitCallInst(CallInst& CI)
{
    if (!CI.getCalledFunction())
    {
        return;
    }

    Value* wiRes = nullptr;

    // Add appropriate sequence and handle out of range where needed
    StringRef funcName = CI.getCalledFunction()->getName();

    if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_ID_X))
    {
        wiRes = getLocalId(CI, ImplicitArg::LOCAL_ID_X);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_ID_Y))
    {
        wiRes = getLocalId(CI, ImplicitArg::LOCAL_ID_Y);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_ID_Z))
    {
        wiRes = getLocalId(CI, ImplicitArg::LOCAL_ID_Z);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_GROUP_ID))
    {
        wiRes = getGroupId(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_THREAD_ID))
    {
        wiRes = getLocalThreadId(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_GLOBAL_SIZE))
    {
        wiRes = getGlobalSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_SIZE))
    {
        wiRes = getLocalSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_ENQUEUED_LOCAL_SIZE)) {
        wiRes = getEnqueuedLocalSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_GLOBAL_OFFSET))
    {
        wiRes = getGlobalOffset(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_WORK_DIM))
    {
        wiRes = getWorkDim(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_NUM_GROUPS))
    {
        wiRes = getNumGroups(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_STAGE_IN_GRID_ORIGIN))
    {
        wiRes = getStageInGridOrigin(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_STAGE_IN_GRID_SIZE))
    {
        wiRes = getStageInGridSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_SYNC_BUFFER))
    {
        wiRes = getSyncBufferPtr(CI);
    }
    else
    {
        // Non WI function, do nothing
        return;
    }

    // Handle size_t return type for 64 bits
    if (wiRes && wiRes->getType()->getScalarSizeInBits() < CI.getType()->getScalarSizeInBits())
    {
        CastInst* pCast = CastInst::Create(Instruction::ZExt, wiRes, IntegerType::get(CI.getContext(), CI.getType()->getScalarSizeInBits()), wiRes->getName(), &CI);
        updateDebugLoc(&CI, pCast);
        wiRes = pCast;
    }

    // Replace original WI call instruction by the result of the appropriate sequence
    if (wiRes) { CI.replaceAllUsesWith(wiRes); }
    CI.eraseFromParent();

    m_changed = true;
}

/************************************************************************************************

R0:

 -----------------------------------------------------------------------------------------------
| Local mem | Group     | Barrier ID| Sampler   | Binding   | Scratch   | Group     | Group     |
| mem index/| number    | /Interface| state     | table     | space     | number    | number    |
| URB handle| X         | descriptor| pointer   | pointer   | pointer   | Y         | Z         |
|           | 32bit     | offset    |           |           |           | 32bit     | 32bit     |
 -----------------------------------------------------------------------------------------------
 <low>                                                                                     <high>


 PayloadHeader:

-----------------------------------------------------------------------------------------------
| Global    | Global    | Global    | Local     | Local     | Local     | Reserved  | Num       |
| offset    | offset    | offset    | size      | size      | size      |           | HW        |
| X         | Y         | Z         | X         | Y         | Z         |           | Threads   |
| 32bit     | 32bit     | 32bit     | 32bit     | 32bit     | 32bit     |           | 32bit     |
 -----------------------------------------------------------------------------------------------
 <low>                                                                                     <high>

*************************************************************************************************/

// Structure of side buffer generated by NEO:
//struct implicit_args {
//    uint8_t struct_size;
//    uint8_t struct_version;
//    uint8_t num_work_dim;
//    uint8_t simd_width;
//    uint32_t local_size_x;
//    uint32_t local_size_y;
//    uint32_t local_size_z;
//    uint64_t global_size_x;
//    uint64_t global_size_y;
//    uint64_t global_size_z;
//    uint64_t printf_buffer_ptr;
//    uint64_t global_offset_x;
//    uint64_t global_offset_y;
//    uint64_t global_offset_z;
//    uint64_t local_id_table_ptr;
//    uint32_t group_count_x;
//    uint32_t group_count_y;
//    uint32_t group_count_z;
//};

// For SIMD8:
//struct local_id_s {
//    uint16_t lx[8];
//    uint16_t reserved[8];
//    uint16_t ly[8];
//    uint16_t reserved[8];
//    uint16_t lz[8];
//    uint16_t reserved[8];
//};

// For SIMD16:
//struct local_id_s {
//    uint16_t lx[16];
//    uint16_t ly[16];
//    uint16_t lz[16];
//};

// For SIMD32:
//struct local_id_s {
//    uint16_t lx[32];
//    uint16_t ly[32];
//    uint16_t lz[32];
//};


class GLOBAL_STATE_FIELD_OFFSETS
{
public:
    // This class holds offsets of various fields in side buffer
    static const uint32_t STRUCT_SIZE = 0;

    static const uint32_t VERSION = STRUCT_SIZE + sizeof(uint8_t);

    static const uint32_t NUM_WORK_DIM = VERSION + sizeof(uint8_t);

    static const uint32_t SIMDSIZE = NUM_WORK_DIM + sizeof(uint8_t);

    static const uint32_t LOCAL_SIZES = SIMDSIZE + sizeof(uint8_t);
    static const uint32_t LOCAL_SIZE_X = LOCAL_SIZES;
    static const uint32_t LOCAL_SIZE_Y = LOCAL_SIZE_X + sizeof(uint32_t);
    static const uint32_t LOCAL_SIZE_Z = LOCAL_SIZE_Y + sizeof(uint32_t);

    static const uint32_t GLOBAL_SIZES = LOCAL_SIZE_Z + sizeof(uint32_t);
    static const uint32_t GLOBAL_SIZE_X = GLOBAL_SIZES;
    static const uint32_t GLOBAL_SIZE_Y = GLOBAL_SIZE_X + sizeof(uint64_t);
    static const uint32_t GLOBAL_SIZE_Z = GLOBAL_SIZE_Y + sizeof(uint64_t);

    static const uint32_t PRINTF_BUFFER = GLOBAL_SIZE_Z + sizeof(uint64_t);

    static const uint32_t GLOBAL_OFFSETS = PRINTF_BUFFER + sizeof(uint64_t);
    static const uint32_t GLOBAL_OFFSET_X = GLOBAL_OFFSETS;
    static const uint32_t GLOBAL_OFFSET_Y = GLOBAL_OFFSET_X + sizeof(uint64_t);
    static const uint32_t GLOBAL_OFFSET_Z = GLOBAL_OFFSET_Y + sizeof(uint64_t);

    static const uint32_t LOCAL_IDS = GLOBAL_OFFSET_Z + sizeof(uint64_t);

    static const uint32_t GROUP_COUNTS = LOCAL_IDS + sizeof(uint64_t);
    static const uint32_t GROUP_COUNT_X = GROUP_COUNTS;
    static const uint32_t GROUP_COUNT_Y = GROUP_COUNT_X + sizeof(uint32_t);
    static const uint32_t GROUP_COUNT_Z = GROUP_COUNT_Y + sizeof(uint32_t);
};

static bool hasStackCallAttr(const llvm::Function& F)
{
    return F.hasFnAttribute("visaStackCall");
}

static Value* BuildLoadInst(CallInst& CI, unsigned int Offset, Type* DataType)
{
    // This function computes type aligned address that includes Offset.
    // Then it loads DataType number of elements from Offset.
    // If Offset is unaligned then it computes aligned offset and loads data.
    // If Offset is unaligned then it copies data to new vector of size <i8 x Size>,
    // bitcasts it to DataType, and returns it.
    // It Offset is aligned, it returns result of LoadInst of type DataType.
    auto ElemByteSize = DataType->getScalarSizeInBits() / 8;
    auto Size = ElemByteSize;
    if (auto DataVecType = dyn_cast<VectorType>(DataType))
    {
        Size *= (unsigned int)DataVecType->getNumElements();
    }
    unsigned int AlignedOffset = (Offset / ElemByteSize) * ElemByteSize;
    unsigned int LoadByteSize = (Offset == AlignedOffset) ? Size : Size * 2;

    llvm::IRBuilder<> Builder(&CI);
    auto F = CI.getFunction();
    auto Int32Ptr = PointerType::get(Type::getInt32Ty(F->getParent()->getContext()), ADDRESS_SPACE_A32);
    auto ElemType = DataType->getScalarType();
    auto LoadType = IGCLLVM::FixedVectorType::get(ElemType, LoadByteSize / ElemByteSize);
    auto PtrType = PointerType::get(LoadType, ADDRESS_SPACE_A32);
    auto IntToPtr = Builder.CreateIntToPtr(Builder.getIntN(F->getParent()->getDataLayout().getPointerSizeInBits(ADDRESS_SPACE_A32), AlignedOffset), Int32Ptr);
    auto BitCast = Builder.CreateBitCast(IntToPtr, PtrType);
    auto LoadInst = Builder.CreateLoad(BitCast);
    LoadInst->setAlignment(IGCLLVM::getCorrectAlign(ElemByteSize));

    if (Offset != AlignedOffset)
    {
        auto ByteType = Type::getInt8Ty(Builder.getContext());
        auto BitCastToByte = Builder.CreateBitCast(LoadInst, ByteType);
        Value* NewVector = UndefValue::get(IGCLLVM::FixedVectorType::get(ByteType, Size));
        for (unsigned int I = Offset; I != (Offset + Size); ++I)
        {
            auto Elem = Builder.CreateExtractElement(BitCastToByte, I - AlignedOffset);
            NewVector = Builder.CreateInsertElement(NewVector, Elem, (uint64_t)I - (uint64_t)Offset);
        }
        auto Result = Builder.CreateBitCast(NewVector, DataType);
        return Result;
    }
    auto Result = Builder.CreateBitCast(LoadInst, DataType);
    return Result;
}

Value* WIFuncResolution::getLocalId(CallInst& CI, ImplicitArg::ArgType argType)
{
    // Receives:
    // call i32 @__builtin_IB_get_local_id_x()

    // Creates:
    // %localIdX

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        // LocalIDBase = oword_ld
        // LocalThreadId = r0.2
        // ThreadBaseOffset = LocalIDBase + LocalThreadId * (SimdSize * 3 * 2)
        // BaseOffset_X = ThreadBaseOffset + 0 * (SimdSize * 2) + (SimdLaneId * 2) OR
        // BaseOffset_Y = ThreadBaseOffset + 1 * (SimdSize * 2) + (SimdLaneId * 2) OR
        // BaseOffset_Z = ThreadBaseOffset + 2 * (SimdSize * 2) + (SimdLaneId * 2)
        // Load from BaseOffset_[X|Y|Z]
        llvm::IRBuilder<> Builder(&CI);

        // Get Local ID Base Ptr
        auto DataTypeI64 = Type::getInt64Ty(F->getParent()->getContext());
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::LOCAL_IDS;
        auto LocalIDBase = BuildLoadInst(CI, Offset, DataTypeI64);

        // Get SIMD Size
        auto DataTypeI32 = Type::getInt32Ty(F->getParent()->getContext());
        auto GetSimdSize = GenISAIntrinsic::getDeclaration(F->getParent(), GenISAIntrinsic::ID::GenISA_simdSize, DataTypeI32);
        llvm::Value* SimdSize = Builder.CreateCall(GetSimdSize);

        // SimdSize = max(SimdSize, 16)
        auto CmpInst = Builder.CreateICmpSGT(SimdSize, ConstantInt::get(SimdSize->getType(), (uint64_t)16));
        SimdSize = Builder.CreateSelect(CmpInst, SimdSize, ConstantInt::get(SimdSize->getType(), (uint64_t)16));

        // Get local thread id
        auto Ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
        VectorType* Tys = IGCLLVM::FixedVectorType::get(DataTypeI32, Ctx->platform.getGRFSize() / SIZE_DWORD);
        Function* R0Dcl = GenISAIntrinsic::getDeclaration(F->getParent(), GenISAIntrinsic::ID::GenISA_getR0, Tys);
        auto IntCall = Builder.CreateCall(R0Dcl);
        auto LocalThreadId = Builder.CreateExtractElement(IntCall, ConstantInt::get(Type::getInt32Ty(CI.getContext()), 2));

        // Get SIMD lane id
        auto DataTypeI16 = Type::getInt16Ty(F->getParent()->getContext());
        auto GetSimdLaneId = GenISAIntrinsic::getDeclaration(F->getParent(), GenISAIntrinsic::ID::GenISA_simdLaneId, DataTypeI16);
        llvm::Value* SimdLaneId = Builder.CreateCall(GetSimdLaneId);

        // Compute thread base offset where local ids for current thread are stored
        // ThreadBaseOffset = LocalIDBasePtr + LocalThreadId * (simd size * 3 * 2)
        auto ThreadBaseOffset = Builder.CreateMul(SimdSize, ConstantInt::get(SimdSize->getType(), (uint64_t)6));
        ThreadBaseOffset = Builder.CreateMul(Builder.CreateZExt(ThreadBaseOffset, LocalThreadId->getType()), LocalThreadId);
        ThreadBaseOffset = Builder.CreateAdd(Builder.CreateZExt(ThreadBaseOffset, LocalIDBase->getType()), LocalIDBase);

        // Compute offset per lane
        uint8_t Factor = 0;
        if (argType == ImplicitArg::ArgType::LOCAL_ID_Y)
        {
            Factor = 2;
        }
        else if (argType == ImplicitArg::ArgType::LOCAL_ID_Z)
        {
            Factor = 4;
        }

        // Compute Factor*(simd size) * 2 to arrive at base of local id for current thread
        auto Expr1 = Builder.CreateMul(SimdSize, ConstantInt::get(SimdSize->getType(), Factor));

        // Compute offset to current lane
        auto Expr2 = Builder.CreateMul(SimdLaneId, ConstantInt::get(SimdLaneId->getType(), 2));

        auto Result = Builder.CreateAdd(Builder.CreateZExt(Expr1, LocalIDBase->getType()),
            Builder.CreateZExt(Expr2, LocalIDBase->getType()));

        Result = Builder.CreateAdd(Result, ThreadBaseOffset);

        // Load data
        auto Int16Ptr = Type::getInt16PtrTy(F->getContext(), 0);
        auto Addr = Builder.CreateIntToPtr(Result, Int16Ptr);
        auto LoadInst = Builder.CreateLoad(Addr);
        auto Trunc = Builder.CreateZExtOrBitCast(LoadInst, CI.getType());
        V = Trunc;
    }
    else
    {
        Argument* localId = getImplicitArg(CI, argType);
        V = localId;
    }

    return V;
}

Value* WIFuncResolution::getGroupId(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_group_id(i32 %dim)

    // Creates:
    // %cmpDim = icmp eq i32 %dim, 0
    // %tmpOffsetR0 = select i1 %cmpDim, i32 1, i32 5
    // %offsetR0 = add i32 %dim, %tmpOffsetR0
    // %groupId = extractelement <8 x i32> %r0, i32 %offsetR0

    // The cmp select insts are present because:
    // if dim = 0 then we need to access R0.1
    // if dim = 1 then we need to access R0.6
    // if dim = 2 then we need to access R0.7

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        auto Ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
        llvm::IRBuilder<> Builder(&CI);
        Type* Int32Ty = Type::getInt32Ty(F->getParent()->getContext());
        VectorType* Tys = IGCLLVM::FixedVectorType::get(Int32Ty, Ctx->platform.getGRFSize() / SIZE_DWORD);
        Function* R0Dcl = GenISAIntrinsic::getDeclaration(F->getParent(), GenISAIntrinsic::ID::GenISA_getR0, Tys);
        auto IntCall = Builder.CreateCall(R0Dcl);
        V = IntCall;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::R0);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* cmpDim = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, dim, ConstantInt::get(Type::getInt32Ty(CI.getContext()), 0), "cmpDim", &CI);
    Instruction* offsetR0 = SelectInst::Create(cmpDim, ConstantInt::get(Type::getInt32Ty(CI.getContext()), 1), ConstantInt::get(Type::getInt32Ty(CI.getContext()), 5), "tmpOffsetR0", &CI);
    Instruction* index = BinaryOperator::CreateAdd(dim, offsetR0, "offsetR0", &CI);
    Instruction* groupId = ExtractElementInst::Create(V, index, "groupId", &CI);
    updateDebugLoc(&CI, cmpDim);
    updateDebugLoc(&CI, offsetR0);
    updateDebugLoc(&CI, index);
    updateDebugLoc(&CI, groupId);

    return groupId;
}
Value* WIFuncResolution::getLocalThreadId(CallInst &CI)
{
    // Receives:
    // call spir_func i32 @__builtin_IB_get_local_thread_id()

    // Creates:
    // %r0second = extractelement <8 x i32> %r0, i32 2
    // %localThreadId = trunc i32 %r0second to i8

    // we need to access R0.2 bits 0 to 7, which contain HW local thread ID on XeHP_SDV+

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        auto Ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
        llvm::IRBuilder<> Builder(&CI);
        Type* Int32Ty = Type::getInt32Ty(F->getParent()->getContext());
        VectorType* Tys = IGCLLVM::FixedVectorType::get(Int32Ty, Ctx->platform.getGRFSize() / SIZE_DWORD);
        Function* R0Dcl = GenISAIntrinsic::getDeclaration(F->getParent(), GenISAIntrinsic::ID::GenISA_getR0, Tys);
        auto IntCall = Builder.CreateCall(R0Dcl);
        V = IntCall;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::R0);
        V = arg;
    }

    Instruction* r0second = ExtractElementInst::Create(V, ConstantInt::get(Type::getInt32Ty(CI.getContext()), 2), "r0second", &CI);
    Instruction* localThreadId = TruncInst::Create(Instruction::CastOps::Trunc, r0second, Type::getInt8Ty(CI.getContext()), "localThreadId", &CI);
    updateDebugLoc(&CI, r0second);
    updateDebugLoc(&CI, localThreadId);

    return localThreadId;
}

Value* WIFuncResolution::getGlobalSize(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_global_size(i32 %dim)

    // Creates:
    // %globalSize1 = extractelement <3 x i32> %globalSize, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeQ = Type::getInt64Ty(F->getParent()->getContext());
        auto VecTyQ = IGCLLVM::FixedVectorType::get(ElemTypeQ, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GLOBAL_SIZE_X;
        auto LoadInst = BuildLoadInst(CI, Offset, VecTyQ);
        auto ElemType = CI.getType();
        Value* Undef = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, 3));
        for (unsigned int I = 0; I != 3; ++I)
        {
            // Extract each dimension, truncate to i32, then insert in new vector
            auto Elem = Builder.CreateExtractElement(LoadInst, (uint64_t)I);
            auto TruncElem = Builder.CreateTrunc(Elem, ElemType);
            Undef = Builder.CreateInsertElement(Undef, TruncElem, (uint64_t)I);
        }
        V = Undef;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::GLOBAL_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* globalSize = ExtractElementInst::Create(V, dim, "globalSize", &CI);
    updateDebugLoc(&CI, globalSize);

    return globalSize;
}

Value* WIFuncResolution::getLocalSize(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_local_size(i32 %dim)

    // Creates:
    // %localSize = extractelement <3 x i32> %localSize, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeD = Type::getInt32Ty(F->getParent()->getContext());
        auto VecTyD = IGCLLVM::FixedVectorType::get(ElemTypeD, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::LOCAL_SIZE_X;
        auto LoadInst = BuildLoadInst(CI, Offset, VecTyD);
        V = LoadInst;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::LOCAL_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* localSize = ExtractElementInst::Create(V, dim, "localSize", &CI);
    updateDebugLoc(&CI, localSize);

    return localSize;
}

Value* WIFuncResolution::getEnqueuedLocalSize(CallInst& CI) {
    // Receives:
    // call i32 @__builtin_IB_get_enqueued_local_size(i32 %dim)

    // Creates:
    // %enqueuedLocalSize1 = extractelement <3 x i32> %enqueuedLocalSize, %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        // Assume that enqueued local size is same as local size
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeD = Type::getInt32Ty(F->getParent()->getContext());
        auto VecTyD = IGCLLVM::FixedVectorType::get(ElemTypeD, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::LOCAL_SIZE_X;
        auto LoadInst = BuildLoadInst(CI, Offset, VecTyD);
        V = LoadInst;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::ENQUEUED_LOCAL_WORK_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* enqueuedLocalSize = ExtractElementInst::Create(V, dim, "enqueuedLocalSize", &CI);
    updateDebugLoc(&CI, enqueuedLocalSize);

    return enqueuedLocalSize;
}

Value* WIFuncResolution::getGlobalOffset(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_global_offset(i32 %dim)

    // Creates:
    // %globalOffset = extractelement <8 x i32> %payloadHeader, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeQ = Type::getInt64Ty(F->getParent()->getContext());
        auto VecTyQ = IGCLLVM::FixedVectorType::get(ElemTypeQ, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GLOBAL_OFFSET_X;
        auto LoadInst = BuildLoadInst(CI, Offset, VecTyQ);
        auto ElemType = CI.getType();
        Value* Undef = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, 3));
        for (unsigned int I = 0; I != 3; ++I)
        {
            // Extract each dimension, truncate to i32, then insert in new vector
            auto Elem = Builder.CreateExtractElement(LoadInst, (uint64_t)I);
            auto TruncElem = Builder.CreateTrunc(Elem, ElemType);
            Undef = Builder.CreateInsertElement(Undef, TruncElem, (uint64_t)I);
        }
        V = Undef;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::PAYLOAD_HEADER);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    auto globalOffset = ExtractElementInst::Create(V, dim, "globalOffset", &CI);
    updateDebugLoc(&CI, cast<Instruction>(globalOffset));

    return globalOffset;
}

Value* WIFuncResolution::getWorkDim(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_work_dim()

    // Creates:
    // %workDim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        unsigned int Size = 4;
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::NUM_WORK_DIM / Size;
        auto TypeUD = Type::getInt32Ty(F->getParent()->getContext());
        auto LoadInst = BuildLoadInst(CI, Offset, TypeUD);
        auto LShr = Builder.CreateLShr(LoadInst, (uint64_t)24);
        V = LShr;
    }
    else
    {
        Argument* workDim = getImplicitArg(CI, ImplicitArg::WORK_DIM);
        V = workDim;
    }

    return V;
}

Value* WIFuncResolution::getNumGroups(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_num_groups(i32 %dim)

    // Creates:
    // %numGroups1 = extractelement <3 x i32> %numGroups, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeUD = Type::getInt32Ty(F->getParent()->getContext());
        auto VecTyUD = IGCLLVM::FixedVectorType::get(ElemTypeUD, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GROUP_COUNT_X;
        auto LoadInst = BuildLoadInst(CI, Offset, VecTyUD);
        V = LoadInst;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::NUM_GROUPS);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* numGroups = ExtractElementInst::Create(V, dim, "numGroups", &CI);
    updateDebugLoc(&CI, numGroups);

    return numGroups;
}

Value* WIFuncResolution::getStageInGridOrigin(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_grid_origin(i32 %dim)

    // Creates:
    // %grid_origin1 = extractelement <3 x i32> %globalSize, i32 %dim

    Argument* arg = getImplicitArg(CI, ImplicitArg::STAGE_IN_GRID_ORIGIN);

    Value* dim = CI.getArgOperand(0);
    Instruction* globalSize = ExtractElementInst::Create(arg, dim, "grid_origin", &CI);
    updateDebugLoc(&CI, globalSize);

    return globalSize;
}

Value* WIFuncResolution::getStageInGridSize(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_grid_size(i32 %dim)

    // Creates:
    // %grid_size1 = extractelement <3 x i32> %globalSize, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeQ = Type::getInt64Ty(F->getParent()->getContext());
        auto VecTyQ = IGCLLVM::FixedVectorType::get(ElemTypeQ, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GLOBAL_SIZE_X;
        auto LoadInst = BuildLoadInst(CI, Offset, VecTyQ);
        auto ElemType = Type::getInt32Ty(F->getParent()->getContext());
        Value* Undef = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, 3));
        for (unsigned int I = 0; I != 3; ++I)
        {
            // Extract each dimension, truncate to i32, then insert in new vector
            auto Elem = Builder.CreateExtractElement(LoadInst, (uint64_t)I);
            auto TruncElem = Builder.CreateTrunc(Elem, ElemType);
            Undef = Builder.CreateInsertElement(Undef, TruncElem, (uint64_t)I);
        }
        V = Undef;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::STAGE_IN_GRID_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* globalSize = ExtractElementInst::Create(V, dim, "grid_size", &CI);
    updateDebugLoc(&CI, globalSize);

    return globalSize;
}

Value* WIFuncResolution::getSyncBufferPtr(CallInst& CI)
{
    // Receives:
    // call i8 addrspace(1)* @__builtin_IB_get_sync_buffer()

    // Creates:
    // i8 addrspace(1)* %syncBuffer

    Argument* syncBuffer = getImplicitArg(CI, ImplicitArg::SYNC_BUFFER);

    return syncBuffer;
}

Argument* WIFuncResolution::getImplicitArg(CallInst& CI, ImplicitArg::ArgType argType)
{
    unsigned int numImplicitArgs = m_implicitArgs.size();
    unsigned int implicitArgIndex = m_implicitArgs.getArgIndex(argType);

    Function* pFunc = CI.getParent()->getParent();
    IGC_ASSERT_MESSAGE(pFunc->arg_size() >= numImplicitArgs, "Function arg size does not match meta data args.");
    unsigned int implicitArgIndexInFunc = pFunc->arg_size() - numImplicitArgs + implicitArgIndex;

    Function::arg_iterator arg = pFunc->arg_begin();
    for (unsigned int i = 0; i < implicitArgIndexInFunc; ++i, ++arg);

    return &(*arg);
}
