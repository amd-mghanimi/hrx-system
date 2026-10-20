// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/wait_dependency_visit.h"

#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

using ::testing::ElementsAre;

class WaitDependencyVisitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
    IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_initialize(
        /*value_count=*/4, &arena_, &visit_));
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  std::vector<loom_amdgpu_wait_dependency_visit_range_t> PopAll() {
    std::vector<loom_amdgpu_wait_dependency_visit_range_t> ranges;
    loom_amdgpu_wait_dependency_visit_range_t range;
    while (loom_amdgpu_wait_dependency_visit_pop(&visit_, &range)) {
      ranges.push_back(range);
    }
    return ranges;
  }

  iree_arena_block_pool_t pool_;
  iree_arena_allocator_t arena_;
  loom_amdgpu_wait_dependency_visit_t visit_;
};

MATCHER_P3(IsRange, value_ordinal, unit_offset, unit_count, "") {
  return arg.value_ordinal == value_ordinal && arg.unit_offset == unit_offset &&
         arg.unit_count == unit_count;
}

TEST_F(WaitDependencyVisitTest, WholeValueUsesDenseDeduplication) {
  loom_amdgpu_wait_dependency_visit_begin(&visit_);
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/1, /*value_unit_count=*/8,
      /*unit_offset=*/0, /*unit_count=*/8));
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/1, /*value_unit_count=*/8,
      /*unit_offset=*/0, /*unit_count=*/8));
  EXPECT_THAT(PopAll(), ElementsAre(IsRange(1, 0, 8)));
  EXPECT_EQ(visit_.coverage_count, 0u);
}

TEST_F(WaitDependencyVisitTest, RetainsOnlyUncoveredDisjointRanges) {
  loom_amdgpu_wait_dependency_visit_begin(&visit_);
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/2, /*value_unit_count=*/10,
      /*unit_offset=*/6, /*unit_count=*/2));
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/2, /*value_unit_count=*/10,
      /*unit_offset=*/2, /*unit_count=*/2));
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/2, /*value_unit_count=*/10,
      /*unit_offset=*/3, /*unit_count=*/4));
  loom_amdgpu_wait_dependency_visit_reverse(&visit_, 0);
  EXPECT_THAT(PopAll(), ElementsAre(IsRange(2, 6, 2), IsRange(2, 2, 2),
                                    IsRange(2, 4, 2)));

  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/2, /*value_unit_count=*/10,
      /*unit_offset=*/2, /*unit_count=*/6));
  EXPECT_TRUE(PopAll().empty());
}

TEST_F(WaitDependencyVisitTest, WholeValueSupersedesPartialCoverage) {
  loom_amdgpu_wait_dependency_visit_begin(&visit_);
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/0, /*value_unit_count=*/8,
      /*unit_offset=*/2, /*unit_count=*/2));
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/0, /*value_unit_count=*/8,
      /*unit_offset=*/0, /*unit_count=*/8));
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/0, /*value_unit_count=*/8,
      /*unit_offset=*/4, /*unit_count=*/2));
  EXPECT_THAT(PopAll(), ElementsAre(IsRange(0, 4, 4), IsRange(0, 0, 2),
                                    IsRange(0, 2, 2)));
}

TEST_F(WaitDependencyVisitTest, TerminalValuePublishesOnceAcrossRanges) {
  loom_amdgpu_wait_dependency_visit_begin(&visit_);
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/0, /*value_unit_count=*/8,
      /*unit_offset=*/0, /*unit_count=*/2));
  EXPECT_TRUE(loom_amdgpu_wait_dependency_visit_complete_value(&visit_, 0));
  EXPECT_FALSE(loom_amdgpu_wait_dependency_visit_complete_value(&visit_, 0));
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/0, /*value_unit_count=*/8,
      /*unit_offset=*/4, /*unit_count=*/2));
  EXPECT_THAT(PopAll(), ElementsAre(IsRange(0, 0, 2)));
}

TEST_F(WaitDependencyVisitTest, NewEpochReusesCapacityAndForgetsCoverage) {
  loom_amdgpu_wait_dependency_visit_begin(&visit_);
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/3, /*value_unit_count=*/16,
      /*unit_offset=*/4, /*unit_count=*/4));
  ASSERT_EQ(PopAll().size(), 1u);
  const iree_host_size_t worklist_capacity = visit_.worklist_capacity;
  const iree_host_size_t coverage_capacity = visit_.coverage_capacity;

  loom_amdgpu_wait_dependency_visit_begin(&visit_);
  IREE_ASSERT_OK(loom_amdgpu_wait_dependency_visit_push(
      &visit_, /*value_ordinal=*/3, /*value_unit_count=*/16,
      /*unit_offset=*/4, /*unit_count=*/4));
  EXPECT_THAT(PopAll(), ElementsAre(IsRange(3, 4, 4)));
  EXPECT_EQ(visit_.worklist_capacity, worklist_capacity);
  EXPECT_EQ(visit_.coverage_capacity, coverage_capacity);
}

}  // namespace
