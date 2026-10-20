// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Materializes task library entry points as ordinary physical Low functions.

#ifndef LOOM_TARGET_ARCH_X86_HAL_MATERIALIZATION_H_
#define LOOM_TARGET_ARCH_X86_HAL_MATERIALIZATION_H_

#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the library query function and its concrete compiler version before
// source lowering. Existing authored library queries remain ordinary functions.
iree_status_t loom_x86_materialize_hal_query_run(loom_pass_t* pass,
                                                 loom_module_t* module);

// Replaces a logical kernel entry with the three-pointer task dispatch ABI.
// The original logical parameter layout remains available for reflection.
iree_status_t loom_x86_materialize_hal_kernel_run(loom_pass_t* pass,
                                                  loom_module_t* module,
                                                  loom_func_like_t function);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_X86_HAL_MATERIALIZATION_H_
