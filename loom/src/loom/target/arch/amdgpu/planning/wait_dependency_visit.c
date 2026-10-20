// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/wait_dependency_visit.h"

#include <string.h>

#define LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE UINT32_MAX
#define LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_WHOLE (UINT32_MAX - 1u)
#define LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_TERMINAL (UINT32_MAX - 2u)

static iree_status_t loom_amdgpu_wait_dependency_visit_append_work(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_value_ordinal_t value_ordinal, uint32_t unit_offset,
    uint32_t unit_count) {
  const iree_host_size_t minimum_capacity =
      iree_max(visit->worklist_count + 1, (iree_host_size_t)16);
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      visit->arena, visit->worklist_count, minimum_capacity,
      sizeof(*visit->worklist), &visit->worklist_capacity,
      (void**)&visit->worklist));
  visit->worklist[visit->worklist_count++] =
      (loom_amdgpu_wait_dependency_visit_range_t){
          .value_ordinal = value_ordinal,
          .unit_offset = unit_offset,
          .unit_count = unit_count,
      };
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_wait_dependency_visit_allocate_coverage(
    loom_amdgpu_wait_dependency_visit_t* visit, uint32_t* out_index) {
  if (visit->coverage_count ==
      LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_TERMINAL) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "wait dependency range coverage exceeds the compact index domain");
  }
  const iree_host_size_t minimum_capacity =
      iree_max(visit->coverage_count + 1, (iree_host_size_t)16);
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      visit->arena, visit->coverage_count, minimum_capacity,
      sizeof(*visit->coverages), &visit->coverage_capacity,
      (void**)&visit->coverages));
  *out_index = (uint32_t)visit->coverage_count++;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_wait_dependency_visit_initialize(
    iree_host_size_t value_count, iree_arena_allocator_t* arena,
    loom_amdgpu_wait_dependency_visit_t* out_visit) {
  *out_visit = (loom_amdgpu_wait_dependency_visit_t){
      .arena = arena,
      .value_count = value_count,
  };
  if (value_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, value_count, sizeof(*out_visit->value_epochs),
      (void**)&out_visit->value_epochs));
  memset(out_visit->value_epochs, 0,
         value_count * sizeof(*out_visit->value_epochs));
  return iree_arena_allocate_array(arena, value_count,
                                   sizeof(*out_visit->coverage_heads),
                                   (void**)&out_visit->coverage_heads);
}

void loom_amdgpu_wait_dependency_visit_begin(
    loom_amdgpu_wait_dependency_visit_t* visit) {
  if (visit->epoch == UINT32_MAX) {
    memset(visit->value_epochs, 0,
           visit->value_count * sizeof(*visit->value_epochs));
    visit->epoch = 0;
  }
  ++visit->epoch;
  visit->worklist_count = 0;
  visit->coverage_count = 0;
}

iree_status_t loom_amdgpu_wait_dependency_visit_push(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_value_ordinal_t value_ordinal, uint32_t value_unit_count,
    uint32_t unit_offset, uint32_t unit_count) {
  IREE_ASSERT_NE(visit->epoch, 0u);
  IREE_ASSERT_LT(value_ordinal, visit->value_count);
  IREE_ASSERT_LE(unit_offset, value_unit_count);
  IREE_ASSERT_LE(unit_count, value_unit_count - unit_offset);
  if (unit_count == 0) {
    return iree_ok_status();
  }

  const bool value_initialized =
      visit->value_epochs[value_ordinal] == visit->epoch;
  uint32_t head_index = value_initialized
                            ? visit->coverage_heads[value_ordinal]
                            : LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE;
  if (value_initialized &&
      head_index == LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_TERMINAL) {
    return iree_ok_status();
  }
  if (unit_offset == 0 && unit_count == value_unit_count) {
    if (value_initialized &&
        head_index == LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_WHOLE) {
      return iree_ok_status();
    }
    if (value_initialized) {
      uint32_t cursor = 0;
      uint32_t coverage_index = head_index;
      while (coverage_index !=
             LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE) {
        const loom_amdgpu_wait_dependency_visit_coverage_t* coverage =
            &visit->coverages[coverage_index];
        if (coverage->unit_offset > cursor) {
          IREE_RETURN_IF_ERROR(loom_amdgpu_wait_dependency_visit_append_work(
              visit, value_ordinal, cursor, coverage->unit_offset - cursor));
        }
        cursor = coverage->unit_offset + coverage->unit_count;
        coverage_index = coverage->next_index;
      }
      if (cursor < value_unit_count) {
        IREE_RETURN_IF_ERROR(loom_amdgpu_wait_dependency_visit_append_work(
            visit, value_ordinal, cursor, value_unit_count - cursor));
      }
      visit->coverage_heads[value_ordinal] =
          LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_WHOLE;
      return iree_ok_status();
    }
    visit->value_epochs[value_ordinal] = visit->epoch;
    visit->coverage_heads[value_ordinal] =
        LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_WHOLE;
    return loom_amdgpu_wait_dependency_visit_append_work(
        visit, value_ordinal, unit_offset, unit_count);
  }
  if (value_initialized &&
      head_index == LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_WHOLE) {
    return iree_ok_status();
  }
  if (!value_initialized) {
    visit->value_epochs[value_ordinal] = visit->epoch;
    visit->coverage_heads[value_ordinal] =
        LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE;
  }

  const uint32_t range_end = unit_offset + unit_count;
  uint32_t cursor = unit_offset;
  uint32_t coverage_index = head_index;
  while (coverage_index != LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE) {
    const loom_amdgpu_wait_dependency_visit_coverage_t* coverage =
        &visit->coverages[coverage_index];
    const uint32_t coverage_end = coverage->unit_offset + coverage->unit_count;
    if (coverage_end <= cursor) {
      coverage_index = coverage->next_index;
      continue;
    }
    if (coverage->unit_offset >= range_end) {
      break;
    }
    if (coverage->unit_offset > cursor) {
      IREE_RETURN_IF_ERROR(loom_amdgpu_wait_dependency_visit_append_work(
          visit, value_ordinal, cursor, coverage->unit_offset - cursor));
    }
    cursor = iree_max(cursor, coverage_end);
    if (cursor >= range_end) {
      break;
    }
    coverage_index = coverage->next_index;
  }
  if (cursor < range_end) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_wait_dependency_visit_append_work(
        visit, value_ordinal, cursor, range_end - cursor));
  }

  uint32_t previous_index = LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE;
  coverage_index = head_index;
  while (coverage_index != LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE) {
    const loom_amdgpu_wait_dependency_visit_coverage_t* coverage =
        &visit->coverages[coverage_index];
    const uint32_t coverage_end = coverage->unit_offset + coverage->unit_count;
    if (coverage_end >= unit_offset) {
      break;
    }
    previous_index = coverage_index;
    coverage_index = coverage->next_index;
  }

  uint32_t merged_offset = unit_offset;
  uint32_t merged_end = range_end;
  uint32_t merged_index = coverage_index;
  if (coverage_index == LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE ||
      visit->coverages[coverage_index].unit_offset > range_end) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_wait_dependency_visit_allocate_coverage(
        visit, &merged_index));
    visit->coverages[merged_index].next_index = coverage_index;
  } else {
    merged_offset =
        iree_min(merged_offset, visit->coverages[coverage_index].unit_offset);
    while (coverage_index != LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE &&
           visit->coverages[coverage_index].unit_offset <= merged_end) {
      const loom_amdgpu_wait_dependency_visit_coverage_t* coverage =
          &visit->coverages[coverage_index];
      merged_end =
          iree_max(merged_end, coverage->unit_offset + coverage->unit_count);
      coverage_index = coverage->next_index;
    }
    visit->coverages[merged_index].next_index = coverage_index;
  }
  visit->coverages[merged_index].unit_offset = merged_offset;
  visit->coverages[merged_index].unit_count = merged_end - merged_offset;
  if (previous_index == LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_NONE) {
    visit->coverage_heads[value_ordinal] = merged_index;
  } else {
    visit->coverages[previous_index].next_index = merged_index;
  }
  return iree_ok_status();
}

bool loom_amdgpu_wait_dependency_visit_complete_value(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_value_ordinal_t value_ordinal) {
  IREE_ASSERT_NE(visit->epoch, 0u);
  IREE_ASSERT_LT(value_ordinal, visit->value_count);
  if (visit->value_epochs[value_ordinal] == visit->epoch &&
      visit->coverage_heads[value_ordinal] ==
          LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_TERMINAL) {
    return false;
  }
  visit->value_epochs[value_ordinal] = visit->epoch;
  visit->coverage_heads[value_ordinal] =
      LOOM_AMDGPU_WAIT_DEPENDENCY_VISIT_COVERAGE_TERMINAL;
  return true;
}

void loom_amdgpu_wait_dependency_visit_reverse(
    loom_amdgpu_wait_dependency_visit_t* visit, iree_host_size_t begin) {
  IREE_ASSERT_LE(begin, visit->worklist_count);
  iree_host_size_t end = visit->worklist_count;
  while (end > begin + 1) {
    loom_amdgpu_wait_dependency_visit_range_t temporary =
        visit->worklist[begin];
    visit->worklist[begin++] = visit->worklist[--end];
    visit->worklist[end] = temporary;
  }
}

bool loom_amdgpu_wait_dependency_visit_pop(
    loom_amdgpu_wait_dependency_visit_t* visit,
    loom_amdgpu_wait_dependency_visit_range_t* out_range) {
  if (visit->worklist_count == 0) {
    return false;
  }
  *out_range = visit->worklist[--visit->worklist_count];
  return true;
}
