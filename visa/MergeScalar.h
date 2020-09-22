/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#ifndef _MERGESCALAR_H
#define _MERGESCALAR_H

#include "G4_Opcode.h"
#include "Gen4_IR.hpp"
#include "BuildIR.h"
#include "FlowGraph.h"

// use by mergeScalar
#define OPND_PATTERN_ENUM(DO) \
    DO(UNKNOWN) \
    DO(IDENTICAL) \
    DO(CONTIGUOUS) \
    DO(DISJOINT)

enum OPND_PATTERN
{
    OPND_PATTERN_ENUM(MAKE_ENUM)
};

static const char* patternNames[] =
{
    OPND_PATTERN_ENUM(STRINGIFY)
};

struct BUNDLE_INFO
{
    static constexpr int maxBundleSize = 16;
    static constexpr int maxNumSrc = 3;
    int size;
    int sizeLimit;
    G4_BB* bb;
    INST_LIST_ITER startIter;
    G4_INST* inst[maxBundleSize];
    OPND_PATTERN dstPattern;
    OPND_PATTERN srcPattern[maxNumSrc];

    BUNDLE_INFO(G4_BB* instBB, INST_LIST_ITER& instPos, int limit) : sizeLimit(limit), bb(instBB)
    {

        inst[0] = *instPos;
        startIter = instPos;
        dstPattern = OPND_PATTERN::UNKNOWN;
        for (int i = 0; i < maxNumSrc; i++)
        {
            srcPattern[i] = OPND_PATTERN::UNKNOWN;
        }
        size = 1;
    }

    void* operator new(size_t sz, Mem_Manager& m) { return m.alloc(sz); }

    void appendInst(G4_INST* lastInst)
    {
        MUST_BE_TRUE(size < maxBundleSize, "max bundle size exceeded");
        inst[size++] = lastInst;
    }

    void deleteLastInst()
    {
        assert(size > 0 && "empty bundle");
        inst[--size] = nullptr;
    }

    bool canMergeDst(G4_DstRegRegion* dst);
    bool canMergeSource(G4_Operand* src, int srcPos);
    bool canMerge(G4_INST* inst);

    bool doMerge(IR_Builder& builder,
        std::unordered_set<G4_Declare*>& modifiedDcl,
        std::vector<G4_Declare*>& newInputs);

    void print(std::ostream& output) const
    {
        output << "Bundle:\n";
        output << "Dst pattern:\t" << patternNames[dstPattern] << "\n";
        output << "Src Pattern:\t";
        for (int i = 0; i < inst[0]->getNumSrc(); ++i)
        {
            output << patternNames[srcPattern[i]] << " ";
        }
        output << "\n";
        for (int i = 0; i < size; ++i)
        {
            inst[i]->emit(output);
            output << "\n";
        }
    }

    void dump() const
    {
        print(std::cerr);
    }

    void findInstructionToMerge(INST_LIST_ITER& iter, const IR_Builder& builder);

    static bool isMergeCandidate(G4_INST* inst, const IR_Builder& builder, bool isInSimdFlow);

};

#endif // _MERGESCALAR_H
