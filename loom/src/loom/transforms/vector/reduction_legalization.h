// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Vector-preserving reduction legalization.

#ifndef LOOM_TRANSFORMS_VECTOR_REDUCTION_LEGALIZATION_H_
#define LOOM_TRANSFORMS_VECTOR_REDUCTION_LEGALIZATION_H_

#include "loom/rewrite/rewriter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rewrites a verified static partial vector.reduce.axes as a fold of
// result-shaped vector slices. Reduced axes are moved before retained axes so
// every fold term is contiguous, while retained multidimensional results are
// flattened only for the lanewise combining operations. Dynamic shapes and
// scalar results remain unchanged for their runtime-loop and vector.reduce
// lowering paths.
iree_status_t loom_vector_reduce_axes_to_vector_rewrite_op(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_rewritten);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_REDUCTION_LEGALIZATION_H_
