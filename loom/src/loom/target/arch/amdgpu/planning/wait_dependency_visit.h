// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Sparse exact-range traversal state for AMDGPU wait dependencies.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_DEPENDENCY_VISIT_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_DEPENDENCY_VISIT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// One exact half-open value-unit range awaiting dependency projection.
typedef struct loom_amdgpu_wait_dependency_visit_range_t {
  // Function-local value ordinal.
  loom_value_ordinal_t value_ordinal;
  // First value unit in the range.
  uint32_t unit_offset;
  // Number of consecutive value units in the range.
  uint32_t unit_count;
} loom_amdgpu_wait_dependency_visit_range_t;

static_assert(sizeof(loom_amdgpu_wait_dependency_visit_range_t) == 12,
              "dependency work rows must retain their compact shape");

// One covered partial range in a value-local sorted union.
typedef struct loom_amdgpu_wait_dependency_visit_coverage_t {
  // First covered value unit.
  uint32_t unit_offset;
  // Number of consecutive covered value units.
  uint32_t unit_count;
  // Next coverage row for the same value, or UINT32_MAX.
  uint32_t next_index;
} loom_amdgpu_wait_dependency_visit_coverage_t;

static_assert(sizeof(loom_amdgpu_wait_dependency_visit_coverage_t) == 12,
              "dependency coverage rows must retain their compact shape");

// Reusable sparse traversal state. Dense value rows are epoch-initialized;
// worklist and partial-coverage capacities persist across independent visits.
typedef struct loom_amdgpu_wait_dependency_visit_t {
  // Arena owning all traversal storage.
  iree_arena_allocator_t* arena;
  // Number of value ordinals addressable by the dense indexes.
  iree_host_size_t value_count;
  // Visit epoch for each value's |coverage_heads| entry.
  uint32_t* value_epochs;
  // Partial coverage head, a whole/terminal sentinel, or UINT32_MAX.
  uint32_t* coverage_heads;
  // Pending exact ranges used as a LIFO worklist.
  loom_amdgpu_wait_dependency_visit_range_t* worklist;
  // Number of pending worklist rows.
  iree_host_size_t worklist_count;
  // Number of allocated worklist rows.
  iree_host_size_t worklist_capacity;
  // Sparse partial-coverage rows for the current visit.
  loom_amdgpu_wait_dependency_visit_coverage_t* coverages;
  // Number of initialized coverage rows.
  iree_host_size_t coverage_count;
  // Number of allocated coverage rows.
  iree_host_size_t coverage_capacity;
  // Current non-zero visit epoch.
  uint32_t epoch;
} loom_amdgpu_wait_dependency_visit_t;

// Initializes reusable traversal state for |value_count| local values.
iree_status_t loom_amdgpu_wait_dependency_visit_initialize(
    iree_host_size_t value_count, iree_arena_allocator_t* arena,
    loom_amdgpu_wait_dependency_visit_t* out_visit);

// Begins an independent traversal and invalidates prior coverage in O(1).
void loom_amdgpu_wait_dependency_visit_begin(
    loom_amdgpu_wait_dependency_visit_t* visit);

// Adds the portions of a valid value-unit range not covered in this visit.
// Whole-value visits use a dense sentinel and allocate no coverage row.
iree_status_t loom_amdgpu_wait_dependency_visit_push(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_value_ordinal_t value_ordinal, uint32_t value_unit_count,
    uint32_t unit_offset, uint32_t unit_count);

// Marks a value as a terminal dependency for this visit. Returns true exactly
// once, allowing all ranges from one SSA producer to publish one direct link.
bool loom_amdgpu_wait_dependency_visit_complete_value(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_value_ordinal_t value_ordinal);

// Reverses pending rows in [begin, worklist_count), preserving source order
// when the rows are subsequently popped from the LIFO worklist.
void loom_amdgpu_wait_dependency_visit_reverse(
    loom_amdgpu_wait_dependency_visit_t* visit, iree_host_size_t begin);

// Pops the next pending exact range, returning false when traversal is done.
bool loom_amdgpu_wait_dependency_visit_pop(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_amdgpu_wait_dependency_visit_range_t* out_range);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_DEPENDENCY_VISIT_H_
