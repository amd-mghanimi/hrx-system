// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/vector/interleave.h"

#include <stdint.h>

static bool loom_vector_interleave_byte_count(
    loom_type_t type, uint16_t physical_element_bit_count,
    uint16_t* out_byte_count) {
  *out_byte_count = 0;
  uint64_t element_count = 0;
  if (!loom_type_static_element_count(type, &element_count) ||
      element_count == 0 || physical_element_bit_count == 0 ||
      (physical_element_bit_count & 7u) != 0 ||
      element_count > UINT64_MAX / physical_element_bit_count) {
    return false;
  }
  const uint64_t byte_count = element_count * physical_element_bit_count / 8u;
  if (byte_count == 0 || byte_count > UINT16_MAX) {
    return false;
  }
  *out_byte_count = (uint16_t)byte_count;
  return true;
}

bool loom_vector_interleave_packet_plan_initialize(
    loom_vector_interleave_kind_t kind, loom_type_t half_type,
    loom_type_t combined_type, int64_t axis,
    uint16_t physical_element_bit_count, uint8_t packet_byte_count,
    uint16_t maximum_packet_count,
    loom_vector_interleave_packet_plan_t* out_plan) {
  *out_plan = (loom_vector_interleave_packet_plan_t){0};
  if ((kind != LOOM_VECTOR_INTERLEAVE_KIND_ZIP &&
       kind != LOOM_VECTOR_INTERLEAVE_KIND_UNZIP) ||
      !loom_type_is_vector(half_type) || !loom_type_is_vector(combined_type) ||
      !loom_type_element_type_equals(half_type, combined_type) ||
      loom_type_rank(half_type) != loom_type_rank(combined_type) ||
      !loom_type_is_all_static(half_type) ||
      !loom_type_is_all_static(combined_type) || packet_byte_count == 0 ||
      axis < 0 || axis >= loom_type_rank(half_type)) {
    return false;
  }

  const uint8_t rank = loom_type_rank(half_type);
  uint64_t trailing_element_count = 1;
  for (uint8_t dimension = 0; dimension < rank; ++dimension) {
    const int64_t half_size =
        loom_type_dim_static_size_at(half_type, dimension);
    const int64_t combined_size =
        loom_type_dim_static_size_at(combined_type, dimension);
    if (half_size < 1 || combined_size < 1 ||
        (dimension == (uint8_t)axis
             ? (half_size > INT64_MAX / 2 || combined_size != half_size * 2)
             : combined_size != half_size)) {
      return false;
    }
    if (dimension > (uint8_t)axis) {
      if (trailing_element_count > UINT64_MAX / (uint64_t)half_size) {
        return false;
      }
      trailing_element_count *= (uint64_t)half_size;
    }
  }

  if (physical_element_bit_count == 0 ||
      (physical_element_bit_count & 7u) != 0 ||
      trailing_element_count > UINT64_MAX / physical_element_bit_count) {
    return false;
  }
  const uint64_t chunk_byte_count =
      trailing_element_count * physical_element_bit_count / 8u;
  if (chunk_byte_count == 0 || chunk_byte_count > UINT16_MAX) {
    return false;
  }

  uint16_t half_byte_count = 0;
  uint16_t combined_byte_count = 0;
  if (!loom_vector_interleave_byte_count(half_type, physical_element_bit_count,
                                         &half_byte_count) ||
      !loom_vector_interleave_byte_count(
          combined_type, physical_element_bit_count, &combined_byte_count) ||
      (uint32_t)half_byte_count * 2u != combined_byte_count) {
    return false;
  }
  const uint16_t source_byte_count = kind == LOOM_VECTOR_INTERLEAVE_KIND_ZIP
                                         ? half_byte_count
                                         : combined_byte_count;
  const uint16_t result_byte_count = kind == LOOM_VECTOR_INTERLEAVE_KIND_ZIP
                                         ? combined_byte_count
                                         : half_byte_count;
  const uint16_t source_packet_count =
      (uint16_t)(((uint32_t)source_byte_count + packet_byte_count - 1u) /
                 packet_byte_count);
  const uint16_t result_packet_count =
      (uint16_t)(((uint32_t)result_byte_count + packet_byte_count - 1u) /
                 packet_byte_count);
  if (source_packet_count == 0 || result_packet_count == 0 ||
      (maximum_packet_count != 0 &&
       (source_packet_count > maximum_packet_count ||
        result_packet_count > maximum_packet_count))) {
    return false;
  }

  *out_plan = (loom_vector_interleave_packet_plan_t){
      .source_byte_count = source_byte_count,
      .result_byte_count = result_byte_count,
      .chunk_byte_count = (uint16_t)chunk_byte_count,
      .source_packet_count = source_packet_count,
      .result_packet_count = result_packet_count,
      .kind = (uint8_t)kind,
      .packet_byte_count = packet_byte_count,
  };
  return true;
}

uint16_t loom_vector_interleave_packet_plan_result_live_byte_count(
    const loom_vector_interleave_packet_plan_t* plan, uint8_t result_index,
    uint16_t result_packet) {
  const uint8_t result_count =
      plan->kind == LOOM_VECTOR_INTERLEAVE_KIND_UNZIP ? 2u : 1u;
  if (result_index >= result_count ||
      result_packet >= plan->result_packet_count ||
      plan->packet_byte_count == 0) {
    return 0;
  }
  const uint32_t result_byte_base =
      (uint32_t)result_packet * plan->packet_byte_count;
  if (result_byte_base >= plan->result_byte_count) {
    return 0;
  }
  return (uint16_t)iree_min(plan->packet_byte_count,
                            plan->result_byte_count - result_byte_base);
}

bool loom_vector_interleave_packet_plan_next_segment(
    const loom_vector_interleave_packet_plan_t* plan, uint8_t result_index,
    uint16_t result_packet, uint16_t* inout_result_packet_byte_offset,
    loom_vector_interleave_packet_segment_t* out_segment) {
  *out_segment = (loom_vector_interleave_packet_segment_t){0};
  const uint16_t live_byte_count =
      loom_vector_interleave_packet_plan_result_live_byte_count(
          plan, result_index, result_packet);
  const uint16_t result_packet_byte = *inout_result_packet_byte_offset;
  if (result_packet_byte >= live_byte_count || plan->chunk_byte_count == 0) {
    return false;
  }

  const uint32_t result_byte =
      (uint32_t)result_packet * plan->packet_byte_count + result_packet_byte;
  const uint32_t chunk = result_byte / plan->chunk_byte_count;
  const uint16_t chunk_byte = (uint16_t)(result_byte % plan->chunk_byte_count);
  uint8_t source_index = 0;
  uint32_t source_byte = 0;
  if (plan->kind == LOOM_VECTOR_INTERLEAVE_KIND_ZIP) {
    source_index = (uint8_t)(chunk & 1u);
    source_byte = (chunk / 2u) * plan->chunk_byte_count + chunk_byte;
  } else {
    source_byte =
        (chunk * 2u + result_index) * plan->chunk_byte_count + chunk_byte;
  }
  const uint16_t source_packet_byte =
      (uint16_t)(source_byte % plan->packet_byte_count);
  const uint16_t byte_count =
      (uint16_t)iree_min(iree_min(plan->chunk_byte_count - chunk_byte,
                                  plan->packet_byte_count - source_packet_byte),
                         live_byte_count - result_packet_byte);
  *out_segment = (loom_vector_interleave_packet_segment_t){
      .result_packet_byte_offset = result_packet_byte,
      .source_packet = (uint16_t)(source_byte / plan->packet_byte_count),
      .source_packet_byte_offset = source_packet_byte,
      .byte_count = byte_count,
      .source_index = source_index,
  };
  *inout_result_packet_byte_offset += byte_count;
  return true;
}
