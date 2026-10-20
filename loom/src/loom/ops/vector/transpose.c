// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/vector/transpose.h"

uint64_t loom_vector_transpose_source_lane(loom_type_t source_type,
                                           loom_type_t result_type,
                                           const int64_t* permutation,
                                           uint64_t result_lane) {
  const uint8_t rank = loom_type_rank(source_type);
  int64_t source_indices[LOOM_TYPE_MAX_RANK] = {0};
  uint64_t remaining = result_lane;
  for (uint8_t reverse_axis = rank; reverse_axis > 0; --reverse_axis) {
    const uint8_t result_axis = reverse_axis - 1;
    const uint64_t dimension_size =
        (uint64_t)loom_type_dim_static_size_at(result_type, result_axis);
    const uint8_t source_axis = (uint8_t)permutation[result_axis];
    source_indices[source_axis] = (int64_t)(remaining % dimension_size);
    remaining /= dimension_size;
  }

  uint64_t source_lane = 0;
  for (uint8_t source_axis = 0; source_axis < rank; ++source_axis) {
    source_lane = source_lane * (uint64_t)loom_type_dim_static_size_at(
                                    source_type, source_axis) +
                  (uint64_t)source_indices[source_axis];
  }
  return source_lane;
}
