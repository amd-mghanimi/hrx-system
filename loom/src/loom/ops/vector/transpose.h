// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared row-major vector.transpose semantics.

#ifndef LOOM_OPS_VECTOR_TRANSPOSE_H_
#define LOOM_OPS_VECTOR_TRANSPOSE_H_

#include <stdint.h>

#include "loom/ir/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the source lane selected for one row-major result lane. The source
// and result must be verified all-static transpose types, |permutation| must
// map each result axis to its source axis, and |result_lane| must be in range.
uint64_t loom_vector_transpose_source_lane(loom_type_t source_type,
                                           loom_type_t result_type,
                                           const int64_t* permutation,
                                           uint64_t result_lane);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_VECTOR_TRANSPOSE_H_
