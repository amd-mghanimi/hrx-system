// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// C/C++ command-line input admission through the public LoomC API.

#ifndef LOOM_IMPORT_CXX_TOOLING_LOOMC_INPUT_H_
#define LOOM_IMPORT_CXX_TOOLING_LOOMC_INPUT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/tooling/io/source_path.h"
#include "loomc/import/cxx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Imports |source| using loom-check/loom-link style provider options. The
// source identifier is the physical main-source path used for relative include
// resolution. The ordinary LoomC result contract distinguishes diagnosed
// source rejection from infrastructure failure.
iree_status_t loom_cxx_input_import_loomc(
    loomc_context_t* context, loomc_workspace_t* workspace,
    const loomc_source_t* source, iree_string_view_t input_options,
    const loom_tooling_source_path_options_t* source_path_options,
    iree_arena_block_pool_t* block_pool, iree_allocator_t host_allocator,
    loomc_module_t** out_module, loomc_result_t** out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IMPORT_CXX_TOOLING_LOOMC_INPUT_H_
