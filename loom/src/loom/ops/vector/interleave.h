// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Static packet routes for vector.interleave and vector.deinterleave.

#ifndef LOOM_OPS_VECTOR_INTERLEAVE_H_
#define LOOM_OPS_VECTOR_INTERLEAVE_H_

#include "iree/base/api.h"
#include "loom/ir/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_vector_interleave_kind_e {
  LOOM_VECTOR_INTERLEAVE_KIND_ZIP = 0,
  LOOM_VECTOR_INTERLEAVE_KIND_UNZIP = 1,
} loom_vector_interleave_kind_t;

// Describes the physical packet layout of one static even/odd permutation.
//
// A zip has two |source_byte_count|-byte sources and one
// |result_byte_count|-byte result. An unzip has one source and two results.
// |chunk_byte_count| is the contiguous row-major payload below the selected
// axis. Packet counts are per source or result value rather than totals across
// both values.
typedef struct loom_vector_interleave_packet_plan_t {
  // Physical bytes in each source value.
  uint16_t source_byte_count;
  // Physical bytes in each result value.
  uint16_t result_byte_count;
  // Physical bytes in one semantic interleave block.
  uint16_t chunk_byte_count;
  // Native packets in each source value.
  uint16_t source_packet_count;
  // Native packets in each result value.
  uint16_t result_packet_count;
  // Zip or unzip semantic direction.
  uint8_t kind;
  // Physical bytes in one native packet.
  uint8_t packet_byte_count;
} loom_vector_interleave_packet_plan_t;
static_assert(sizeof(loom_vector_interleave_packet_plan_t) == 12,
              "interleave packet plans must stay cache dense");

// One contiguous contribution to a result packet. Every byte in the segment
// comes from one physical source packet and one semantic interleave block.
typedef struct loom_vector_interleave_packet_segment_t {
  // Byte offset in the result packet.
  uint16_t result_packet_byte_offset;
  // Source packet ordinal within |source_index|.
  uint16_t source_packet;
  // Byte offset in |source_packet|.
  uint16_t source_packet_byte_offset;
  // Number of contiguous bytes in this segment.
  uint16_t byte_count;
  // Zip source ordinal; always zero for unzip.
  uint8_t source_index;
} loom_vector_interleave_packet_segment_t;

// Initializes a byte-addressable packet plan for the given semantic vector
// shapes. |half_type| is either zip's operand type or unzip's result type;
// |combined_type| is the corresponding zip result or unzip source type.
// |physical_element_bit_count| names the target carrier width of one logical
// element, which may differ from the source bit width for predicates. Returns
// false for dynamic or inconsistent shapes, non-byte-addressable carriers, or
// plans exceeding the explicit packet bound.
bool loom_vector_interleave_packet_plan_initialize(
    loom_vector_interleave_kind_t kind, loom_type_t half_type,
    loom_type_t combined_type, int64_t axis,
    uint16_t physical_element_bit_count, uint8_t packet_byte_count,
    uint16_t maximum_packet_count,
    loom_vector_interleave_packet_plan_t* out_plan);

// Returns the number of meaningful bytes in one result packet, or zero for an
// out-of-range result or packet ordinal.
uint16_t loom_vector_interleave_packet_plan_result_live_byte_count(
    const loom_vector_interleave_packet_plan_t* plan, uint8_t result_index,
    uint16_t result_packet);

// Returns and advances the next segment in a result packet. Initialize
// |inout_result_packet_byte_offset| to zero and call until this returns false.
// |result_index| is zero for zip and selects the even or odd result for unzip.
bool loom_vector_interleave_packet_plan_next_segment(
    const loom_vector_interleave_packet_plan_t* plan, uint8_t result_index,
    uint16_t result_packet, uint16_t* inout_result_packet_byte_offset,
    loom_vector_interleave_packet_segment_t* out_segment);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_VECTOR_INTERLEAVE_H_
