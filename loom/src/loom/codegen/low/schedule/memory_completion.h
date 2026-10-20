// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_CODEGEN_LOW_SCHEDULE_MEMORY_COMPLETION_H_
#define LOOM_CODEGEN_LOW_SCHEDULE_MEMORY_COMPLETION_H_

#include "loom/codegen/low/schedule/context.h"

#ifdef __cplusplus
extern "C" {
#endif

// Retains exact memory completions required across forward CFG edges. Source
// refinement is published on effect-use rows and completion edges are copied
// into the schedule arena; all frontier state remains in the scratch arena.
iree_status_t loom_low_schedule_build_acyclic_memory_completions(
    loom_low_schedule_build_state_t* state);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_SCHEDULE_MEMORY_COMPLETION_H_
