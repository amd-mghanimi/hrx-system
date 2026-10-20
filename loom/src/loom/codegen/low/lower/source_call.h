// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-to-Low normalization for call-like source operations.

#ifndef LOOM_CODEGEN_LOW_LOWER_SOURCE_CALL_H_
#define LOOM_CODEGEN_LOW_LOWER_SOURCE_CALL_H_

#include "iree/base/api.h"
#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_source_invoke_plan_t
    loom_low_lower_source_invoke_plan_t;

// Returns true when |source_op| is a direct semantic CallLike operation owned
// by common source-to-Low lowering. Callable operands and results must occupy
// the complete flat operation boundary.
bool loom_low_lower_source_call_is_structural(const loom_module_t* module,
                                              const loom_op_t* source_op);

// Lowers one direct semantic CallLike operation to low.func.call.
iree_status_t loom_low_lower_source_call(loom_low_lower_context_t* context,
                                         const loom_op_t* source_op);

// Resolves a low.invoke helper contract, checks result carriers, and proves its
// preconditions. Retained predicates preserve each formal's operand position;
// emission does not borrow the helper's signature or source body. Selection has
// already planned the helper's exact Low contract; publication is not required.
iree_status_t loom_low_lower_source_invoke_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_lower_source_invoke_plan_t** out_plan);

// Checks actual producer carriers against the retained helper arguments before
// Low construction. The shared definition-ordered planner owns this boundary.
iree_status_t loom_low_lower_source_invoke_finalize(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_lower_source_invoke_plan_t* plan);

// Normalizes low.invoke using the helper contract and validated carriers.
iree_status_t loom_low_lower_source_invoke(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_lower_source_invoke_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_SOURCE_CALL_H_
