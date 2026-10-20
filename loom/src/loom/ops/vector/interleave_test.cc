// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/vector/interleave.h"

#include <array>
#include <tuple>
#include <vector>

#include "iree/testing/gtest.h"

namespace loom {
namespace {

struct Segment {
  uint16_t result_byte;
  uint8_t source_index;
  uint16_t source_packet;
  uint16_t source_byte;
  uint16_t byte_count;

  bool operator==(const Segment& other) const {
    return std::tie(result_byte, source_index, source_packet, source_byte,
                    byte_count) ==
           std::tie(other.result_byte, other.source_index, other.source_packet,
                    other.source_byte, other.byte_count);
  }
};

static std::vector<Segment> CollectSegments(
    const loom_vector_interleave_packet_plan_t& plan, uint8_t result_index,
    uint16_t result_packet) {
  std::vector<Segment> segments;
  uint16_t cursor = 0;
  loom_vector_interleave_packet_segment_t segment;
  while (loom_vector_interleave_packet_plan_next_segment(
      &plan, result_index, result_packet, &cursor, &segment)) {
    segments.push_back({segment.result_packet_byte_offset, segment.source_index,
                        segment.source_packet,
                        segment.source_packet_byte_offset, segment.byte_count});
  }
  return segments;
}

using ByteRoute = std::pair<uint8_t, uint32_t>;

static std::vector<ByteRoute> CollectByteRoutes(
    const loom_vector_interleave_packet_plan_t& plan, uint8_t result_index) {
  std::vector<ByteRoute> routes;
  for (uint16_t result_packet = 0; result_packet < plan.result_packet_count;
       ++result_packet) {
    const uint16_t live_byte_count =
        loom_vector_interleave_packet_plan_result_live_byte_count(
            &plan, result_index, result_packet);
    uint16_t cursor = 0;
    loom_vector_interleave_packet_segment_t segment;
    while (loom_vector_interleave_packet_plan_next_segment(
        &plan, result_index, result_packet, &cursor, &segment)) {
      EXPECT_GT(segment.byte_count, 0u);
      EXPECT_EQ(segment.result_packet_byte_offset, cursor - segment.byte_count);
      EXPECT_LT(segment.source_packet, plan.source_packet_count);
      EXPECT_LE(segment.source_packet_byte_offset + segment.byte_count,
                plan.packet_byte_count);
      for (uint16_t byte = 0; byte < segment.byte_count; ++byte) {
        routes.emplace_back(
            segment.source_index,
            (uint32_t)segment.source_packet * plan.packet_byte_count +
                segment.source_packet_byte_offset + byte);
      }
    }
    EXPECT_EQ(cursor, live_byte_count);
  }
  return routes;
}

static std::vector<ByteRoute> ReferenceByteRoutes(
    const loom_vector_interleave_packet_plan_t& plan, uint8_t result_index) {
  std::vector<ByteRoute> routes;
  for (uint32_t result_byte = 0; result_byte < plan.result_byte_count;
       ++result_byte) {
    const uint32_t chunk = result_byte / plan.chunk_byte_count;
    const uint16_t chunk_byte = result_byte % plan.chunk_byte_count;
    if (plan.kind == LOOM_VECTOR_INTERLEAVE_KIND_ZIP) {
      routes.emplace_back(chunk & 1u,
                          (chunk / 2u) * plan.chunk_byte_count + chunk_byte);
    } else {
      routes.emplace_back(
          0, (chunk * 2u + result_index) * plan.chunk_byte_count + chunk_byte);
    }
  }
  return routes;
}

TEST(VectorInterleavePlanTest, CoversPhysicalElementFamilies) {
  const std::array<std::pair<loom_scalar_type_t, uint16_t>, 13> families = {{
      {LOOM_SCALAR_TYPE_I1, 32},
      {LOOM_SCALAR_TYPE_I8, 8},
      {LOOM_SCALAR_TYPE_I16, 16},
      {LOOM_SCALAR_TYPE_I32, 32},
      {LOOM_SCALAR_TYPE_I64, 64},
      {LOOM_SCALAR_TYPE_F8E4M3, 8},
      {LOOM_SCALAR_TYPE_F8E5M2, 8},
      {LOOM_SCALAR_TYPE_F16, 16},
      {LOOM_SCALAR_TYPE_BF16, 16},
      {LOOM_SCALAR_TYPE_F32, 32},
      {LOOM_SCALAR_TYPE_F64, 64},
      {LOOM_SCALAR_TYPE_INDEX, 32},
      {LOOM_SCALAR_TYPE_OFFSET, 32},
  }};
  for (auto [element_type, physical_bit_count] : families) {
    const loom_type_t half = loom_type_shaped_1d(LOOM_TYPE_VECTOR, element_type,
                                                 loom_dim_pack_static(3), 0);
    const loom_type_t combined = loom_type_shaped_1d(
        LOOM_TYPE_VECTOR, element_type, loom_dim_pack_static(6), 0);
    for (loom_vector_interleave_kind_t kind : {
             LOOM_VECTOR_INTERLEAVE_KIND_ZIP,
             LOOM_VECTOR_INTERLEAVE_KIND_UNZIP,
         }) {
      loom_vector_interleave_packet_plan_t plan;
      ASSERT_TRUE(loom_vector_interleave_packet_plan_initialize(
          kind, half, combined, /*axis=*/0, physical_bit_count,
          /*packet_byte_count=*/16, /*maximum_packet_count=*/64, &plan));
      EXPECT_EQ(plan.chunk_byte_count, physical_bit_count / 8u);
      EXPECT_EQ(plan.kind, kind);
    }
  }
}

TEST(VectorInterleavePlanTest, AxisSelectsTrailingSemanticBlock) {
  const loom_type_t half =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I16,
                          loom_dim_pack_static(3), loom_dim_pack_static(5), 0);
  const loom_type_t combined_axis0 =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I16,
                          loom_dim_pack_static(6), loom_dim_pack_static(5), 0);
  const loom_type_t combined_axis1 =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I16,
                          loom_dim_pack_static(3), loom_dim_pack_static(10), 0);

  loom_vector_interleave_packet_plan_t axis0_plan;
  ASSERT_TRUE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, combined_axis0, /*axis=*/0,
      /*physical_element_bit_count=*/16, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &axis0_plan));
  EXPECT_EQ(axis0_plan.chunk_byte_count, 10u);

  loom_vector_interleave_packet_plan_t axis1_plan;
  ASSERT_TRUE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, combined_axis1, /*axis=*/1,
      /*physical_element_bit_count=*/16, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &axis1_plan));
  EXPECT_EQ(axis1_plan.chunk_byte_count, 2u);
}

TEST(VectorInterleavePlanTest, RoutesBoundaryMatrix) {
  const std::array<std::pair<loom_scalar_type_t, uint16_t>, 5> families = {{
      {LOOM_SCALAR_TYPE_I8, 8},
      {LOOM_SCALAR_TYPE_I16, 16},
      {LOOM_SCALAR_TYPE_I32, 32},
      {LOOM_SCALAR_TYPE_I64, 64},
      {LOOM_SCALAR_TYPE_I1, 32},
  }};
  for (auto [element_type, physical_bit_count] : families) {
    for (uint8_t packet_byte_count : {16, 64}) {
      for (int64_t trailing_elements : {1, 2, 3, 5, 8, 15, 16, 21}) {
        const loom_type_t half = loom_type_shaped_2d(
            LOOM_TYPE_VECTOR, element_type, loom_dim_pack_static(3),
            loom_dim_pack_static(trailing_elements), 0);
        for (int64_t axis : {0, 1}) {
          const loom_type_t combined = loom_type_shaped_2d(
              LOOM_TYPE_VECTOR, element_type,
              loom_dim_pack_static(axis == 0 ? 6 : 3),
              loom_dim_pack_static(axis == 1 ? trailing_elements * 2
                                             : trailing_elements),
              0);
          for (loom_vector_interleave_kind_t kind : {
                   LOOM_VECTOR_INTERLEAVE_KIND_ZIP,
                   LOOM_VECTOR_INTERLEAVE_KIND_UNZIP,
               }) {
            SCOPED_TRACE(::testing::Message()
                         << "type=" << element_type
                         << " physical_bits=" << physical_bit_count
                         << " packet_bytes=" << (int)packet_byte_count
                         << " trailing_elements=" << trailing_elements
                         << " axis=" << axis << " kind=" << kind);
            loom_vector_interleave_packet_plan_t plan;
            ASSERT_TRUE(loom_vector_interleave_packet_plan_initialize(
                kind, half, combined, axis, physical_bit_count,
                packet_byte_count, /*maximum_packet_count=*/64, &plan));
            const uint8_t result_count =
                kind == LOOM_VECTOR_INTERLEAVE_KIND_UNZIP ? 2 : 1;
            for (uint8_t result_index = 0; result_index < result_count;
                 ++result_index) {
              EXPECT_EQ(CollectByteRoutes(plan, result_index),
                        ReferenceByteRoutes(plan, result_index));
            }
          }
        }
      }
    }
  }
}

TEST(VectorInterleavePlanTest, RoutesIrregularZipAcrossPackets) {
  const loom_type_t half =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(3), loom_dim_pack_static(21), 0);
  const loom_type_t combined =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(6), loom_dim_pack_static(21), 0);
  loom_vector_interleave_packet_plan_t plan;
  ASSERT_TRUE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, combined, /*axis=*/0,
      /*physical_element_bit_count=*/8, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &plan));

  EXPECT_EQ(CollectSegments(plan, /*result_index=*/0, /*result_packet=*/1),
            (std::vector<Segment>{
                {0, 0, 1, 0, 5},
                {5, 1, 0, 0, 11},
            }));
  EXPECT_EQ(CollectSegments(plan, /*result_index=*/0, /*result_packet=*/2),
            (std::vector<Segment>{
                {0, 1, 0, 11, 5},
                {5, 1, 1, 0, 5},
                {10, 0, 1, 5, 6},
            }));
}

TEST(VectorInterleavePlanTest, RoutesBothIrregularUnzipResults) {
  const loom_type_t half =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(3), loom_dim_pack_static(21), 0);
  const loom_type_t combined =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(6), loom_dim_pack_static(21), 0);
  loom_vector_interleave_packet_plan_t plan;
  ASSERT_TRUE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_UNZIP, half, combined, /*axis=*/0,
      /*physical_element_bit_count=*/8, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &plan));

  EXPECT_EQ(CollectSegments(plan, /*result_index=*/0, /*result_packet=*/1),
            (std::vector<Segment>{
                {0, 0, 1, 0, 5},
                {5, 0, 2, 10, 6},
                {11, 0, 3, 0, 5},
            }));
  EXPECT_EQ(CollectSegments(plan, /*result_index=*/1, /*result_packet=*/1),
            (std::vector<Segment>{
                {0, 0, 2, 5, 5},
                {5, 0, 3, 15, 1},
                {6, 0, 4, 0, 10},
            }));
}

TEST(VectorInterleavePlanTest, RejectsInvalidOrUnboundedShapes) {
  const loom_type_t half =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(3), loom_dim_pack_static(21), 0);
  const loom_type_t wrong_shape =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(5), loom_dim_pack_static(21), 0);
  const loom_type_t combined =
      loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I8,
                          loom_dim_pack_static(6), loom_dim_pack_static(21), 0);
  loom_vector_interleave_packet_plan_t plan;
  EXPECT_FALSE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, wrong_shape, /*axis=*/0,
      /*physical_element_bit_count=*/8, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &plan));
  EXPECT_FALSE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, combined, /*axis=*/2,
      /*physical_element_bit_count=*/8, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &plan));
  EXPECT_FALSE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, combined, /*axis=*/0,
      /*physical_element_bit_count=*/1, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/64, &plan));
  EXPECT_FALSE(loom_vector_interleave_packet_plan_initialize(
      LOOM_VECTOR_INTERLEAVE_KIND_ZIP, half, combined, /*axis=*/0,
      /*physical_element_bit_count=*/8, /*packet_byte_count=*/16,
      /*maximum_packet_count=*/1, &plan));
}

}  // namespace
}  // namespace loom
