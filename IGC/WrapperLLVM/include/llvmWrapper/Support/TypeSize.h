/*========================== begin_copyright_notice ============================

Copyright (C) 2020-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef IGCLLVM_SUPPORT_TYPESIZE_H
#define IGCLLVM_SUPPORT_TYPESIZE_H

#if LLVM_VERSION_MAJOR > 10
#include <llvm/Support/TypeSize.h>
using namespace llvm;
#endif

namespace IGCLLVM {
#if LLVM_VERSION_MAJOR < 11
inline unsigned getElementCount(unsigned EC) { return EC; }
#elif LLVM_VERSION_MAJOR < 12
inline ElementCount getElementCount(unsigned EC) {
  return ElementCount(EC, false);
#elif LLVM_VERSION_MAJOR < 13
inline ElementCount getElementCount(unsigned EC) {
  return LinearPolySize<ElementCount>::getFixed(EC);
}
#endif
} // namespace IGCLLVM

#endif
