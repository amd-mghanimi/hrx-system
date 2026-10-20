// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Optional reports projected from finalized AMDGPU memory plans.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_REPORT_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_REPORT_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Appends detail rows for the exact memory instructions selected by |plan|.
// Reporting must be enabled and the source analysis snapshot must still live.
iree_status_t loom_amdgpu_report_memory_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_access_plan_t* plan,
    uint64_t execution_count_plus_one);

// Appends detail rows using the same physical-access enumeration as fragment
// emission, including split loads and cross-lane publication. Reporting must
// be enabled and the source analysis snapshot must still live.
iree_status_t loom_amdgpu_report_fragment_memory_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_fragment_memory_plan_t* plan,
    uint64_t execution_count_plus_one);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_REPORT_H_
