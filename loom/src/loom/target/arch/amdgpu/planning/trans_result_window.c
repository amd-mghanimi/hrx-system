// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/trans_result_window.h"

#include <string.h>

#include "iree/base/bitfield.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/arch/amdgpu/target_info_defs.h"

typedef enum loom_amdgpu_trans_result_window_entry_flag_bits_e {
  LOOM_AMDGPU_TRANS_RESULT_WINDOW_ENTRY_FLAG_ACTIVE = 1u << 0,
} loom_amdgpu_trans_result_window_entry_flag_bits_t;
typedef uint8_t loom_amdgpu_trans_result_window_entry_flags_t;

struct loom_amdgpu_trans_result_window_entry_t {
  // Dense active-list position while ACTIVE is set.
  uint32_t active_list_index;
  // Number of vector ALU packets since the result was produced.
  uint8_t valu_interval;
  // Number of transcendental packets since the result was produced.
  uint8_t trans_interval;
  // Entry state bits.
  loom_amdgpu_trans_result_window_entry_flags_t flags;
  // Explicit padding retained so every entry occupies eight bytes.
  uint8_t reserved;
};

static_assert(sizeof(loom_amdgpu_trans_result_window_entry_t) == 8,
              "TRANS-result window entries must remain compact");
static_assert(sizeof(loom_amdgpu_trans_result_window_t) == 24,
              "TRANS-result windows must remain compact planner state");

static uint32_t* loom_amdgpu_trans_result_window_active_register_indices(
    const loom_amdgpu_trans_result_window_t* window) {
  return (uint32_t*)(window->entries + window->register_count);
}

static uint32_t* loom_amdgpu_trans_result_window_origins(
    const loom_amdgpu_trans_result_window_t* window) {
  if (!iree_any_bit_set(window->flags,
                        LOOM_AMDGPU_TRANS_RESULT_WINDOW_FLAG_TRACK_ORIGINS)) {
    return NULL;
  }
  return loom_amdgpu_trans_result_window_active_register_indices(window) +
         window->register_count;
}

static bool loom_amdgpu_trans_result_window_assignment_is_physical_vgpr(
    const loom_low_allocation_assignment_t* assignment) {
  return assignment != NULL &&
         assignment->location_kind ==
             LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER &&
         assignment->descriptor_reg_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR;
}

static bool loom_amdgpu_trans_result_window_entry_is_active(
    const loom_amdgpu_trans_result_window_entry_t* entry) {
  return iree_any_bit_set(entry->flags,
                          LOOM_AMDGPU_TRANS_RESULT_WINDOW_ENTRY_FLAG_ACTIVE);
}

static uint8_t loom_amdgpu_trans_result_window_saturated_increment(
    uint8_t value, uint8_t limit) {
  return value <= limit ? (uint8_t)(value + 1u) : value;
}

static void loom_amdgpu_trans_result_window_remove(
    loom_amdgpu_trans_result_window_t* window, uint32_t register_index) {
  IREE_ASSERT_LT(register_index, window->register_count);
  loom_amdgpu_trans_result_window_entry_t* entry =
      &window->entries[register_index];
  if (!loom_amdgpu_trans_result_window_entry_is_active(entry)) {
    *entry = (loom_amdgpu_trans_result_window_entry_t){0};
    return;
  }
  IREE_ASSERT_NE(window->active_register_count, 0);
  const uint32_t removed_list_index = entry->active_list_index;
  const uint32_t last_list_index =
      (uint32_t)(window->active_register_count - 1);
  IREE_ASSERT_LT(removed_list_index, window->active_register_count);
  uint32_t* active_register_indices =
      loom_amdgpu_trans_result_window_active_register_indices(window);
  if (removed_list_index != last_list_index) {
    const uint32_t moved_register_index =
        active_register_indices[last_list_index];
    active_register_indices[removed_list_index] = moved_register_index;
    window->entries[moved_register_index].active_list_index =
        removed_list_index;
  }
  --window->active_register_count;
  *entry = (loom_amdgpu_trans_result_window_entry_t){0};
}

static void loom_amdgpu_trans_result_window_activate(
    loom_amdgpu_trans_result_window_t* window, uint32_t register_index) {
  IREE_ASSERT_LT(register_index, window->register_count);
  loom_amdgpu_trans_result_window_entry_t* entry =
      &window->entries[register_index];
  if (loom_amdgpu_trans_result_window_entry_is_active(entry)) {
    return;
  }
  IREE_ASSERT_LT(window->active_register_count, window->register_count);
  const uint32_t active_list_index = (uint32_t)window->active_register_count++;
  loom_amdgpu_trans_result_window_active_register_indices(
      window)[active_list_index] = register_index;
  entry->active_list_index = active_list_index;
}

iree_status_t loom_amdgpu_trans_result_window_initialize(
    iree_host_size_t register_count,
    loom_amdgpu_trans_result_window_flags_t flags,
    iree_arena_allocator_t* arena,
    loom_amdgpu_trans_result_window_t* out_window) {
  *out_window = (loom_amdgpu_trans_result_window_t){0};
  if (register_count == 0) {
    return iree_ok_status();
  }
  IREE_ASSERT_LE(register_count, UINT32_MAX);
  const bool track_origins = iree_any_bit_set(
      flags, LOOM_AMDGPU_TRANS_RESULT_WINDOW_FLAG_TRACK_ORIGINS);
  const iree_host_size_t storage_bytes_per_register =
      sizeof(*out_window->entries) + sizeof(uint32_t) +
      (track_origins ? sizeof(uint32_t) : 0);
  uint8_t* storage = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, register_count, storage_bytes_per_register, (void**)&storage));
  out_window->entries = (loom_amdgpu_trans_result_window_entry_t*)storage;
  memset(out_window->entries, 0, register_count * sizeof(*out_window->entries));
  out_window->register_count = (uint32_t)register_count;
  out_window->flags = flags;
  return iree_ok_status();
}

bool loom_amdgpu_trans_result_window_is_initialized(
    const loom_amdgpu_trans_result_window_t* window) {
  return window->entries != NULL && window->register_count != 0;
}

bool loom_amdgpu_trans_result_window_has_active(
    const loom_amdgpu_trans_result_window_t* window) {
  return loom_amdgpu_trans_result_window_is_initialized(window) &&
         window->active_register_count != 0;
}

void loom_amdgpu_trans_result_window_clear(
    loom_amdgpu_trans_result_window_t* window) {
  if (!loom_amdgpu_trans_result_window_has_active(window)) {
    return;
  }
  const uint32_t* active_register_indices =
      loom_amdgpu_trans_result_window_active_register_indices(window);
  while (window->active_register_count != 0) {
    const uint32_t active_list_index =
        (uint32_t)(window->active_register_count - 1);
    const uint32_t register_index = active_register_indices[active_list_index];
    window->entries[register_index] =
        (loom_amdgpu_trans_result_window_entry_t){0};
    --window->active_register_count;
  }
}

void loom_amdgpu_trans_result_window_advance(
    loom_amdgpu_trans_result_window_t* window,
    loom_amdgpu_trans_result_packet_flags_t packet_flags) {
  if (!loom_amdgpu_trans_result_window_has_active(window) ||
      packet_flags == 0) {
    return;
  }
  const bool is_vector_alu = iree_any_bit_set(
      packet_flags, LOOM_AMDGPU_TRANS_RESULT_PACKET_FLAG_VECTOR_ALU);
  const bool is_transcendental = iree_any_bit_set(
      packet_flags, LOOM_AMDGPU_TRANS_RESULT_PACKET_FLAG_TRANSCENDENTAL);
  const uint32_t* active_register_indices =
      loom_amdgpu_trans_result_window_active_register_indices(window);
  for (iree_host_size_t i = 0; i < window->active_register_count;) {
    const uint32_t register_index = active_register_indices[i];
    loom_amdgpu_trans_result_window_entry_t* entry =
        &window->entries[register_index];
    IREE_ASSERT(loom_amdgpu_trans_result_window_entry_is_active(entry));
    if (is_vector_alu) {
      entry->valu_interval =
          loom_amdgpu_trans_result_window_saturated_increment(
              entry->valu_interval,
              LOOM_AMDGPU_VALU_TRANS_USE_DEPCTR_MAX_VALU_INTERVAL);
    }
    if (is_transcendental) {
      entry->trans_interval =
          loom_amdgpu_trans_result_window_saturated_increment(
              entry->trans_interval,
              LOOM_AMDGPU_VALU_TRANS_USE_DEPCTR_MAX_TRANS_INTERVAL);
    }
    if (entry->valu_interval >
            LOOM_AMDGPU_VALU_TRANS_USE_DEPCTR_MAX_VALU_INTERVAL ||
        entry->trans_interval >
            LOOM_AMDGPU_VALU_TRANS_USE_DEPCTR_MAX_TRANS_INTERVAL) {
      loom_amdgpu_trans_result_window_remove(window, register_index);
      continue;
    }
    ++i;
  }
}

void loom_amdgpu_trans_result_window_clear_assignment(
    loom_amdgpu_trans_result_window_t* window,
    const loom_low_allocation_assignment_t* assignment) {
  if (!loom_amdgpu_trans_result_window_is_initialized(window) ||
      !loom_amdgpu_trans_result_window_assignment_is_physical_vgpr(
          assignment)) {
    return;
  }
  const uint64_t end =
      (uint64_t)assignment->location_base + assignment->location_count;
  IREE_ASSERT_LE(end, window->register_count);
  for (uint32_t i = 0; i < assignment->location_count; ++i) {
    loom_amdgpu_trans_result_window_remove(window,
                                           assignment->location_base + i);
  }
}

void loom_amdgpu_trans_result_window_record_assignment(
    loom_amdgpu_trans_result_window_t* window,
    const loom_low_allocation_assignment_t* assignment, uint32_t origin) {
  if (!loom_amdgpu_trans_result_window_is_initialized(window) ||
      !loom_amdgpu_trans_result_window_assignment_is_physical_vgpr(
          assignment)) {
    return;
  }
  const uint64_t end =
      (uint64_t)assignment->location_base + assignment->location_count;
  IREE_ASSERT_LE(end, window->register_count);
  uint32_t* origins = loom_amdgpu_trans_result_window_origins(window);
  for (uint32_t i = 0; i < assignment->location_count; ++i) {
    const uint32_t register_index = assignment->location_base + i;
    loom_amdgpu_trans_result_window_activate(window, register_index);
    loom_amdgpu_trans_result_window_entry_t* entry =
        &window->entries[register_index];
    const uint32_t active_list_index = entry->active_list_index;
    *entry = (loom_amdgpu_trans_result_window_entry_t){
        .active_list_index = active_list_index,
        .flags = LOOM_AMDGPU_TRANS_RESULT_WINDOW_ENTRY_FLAG_ACTIVE,
    };
    if (origins != NULL) {
      origins[register_index] = origin;
    }
  }
}

bool loom_amdgpu_trans_result_window_query_assignment_origin(
    const loom_amdgpu_trans_result_window_t* window,
    const loom_low_allocation_assignment_t* assignment, uint32_t* out_origin) {
  *out_origin = UINT32_MAX;
  if (!loom_amdgpu_trans_result_window_has_active(window) ||
      !loom_amdgpu_trans_result_window_assignment_is_physical_vgpr(
          assignment)) {
    return false;
  }
  const uint32_t* origins = loom_amdgpu_trans_result_window_origins(window);
  IREE_ASSERT(origins != NULL);
  const uint64_t end =
      (uint64_t)assignment->location_base + assignment->location_count;
  IREE_ASSERT_LE(end, window->register_count);
  for (uint32_t i = 0; i < assignment->location_count; ++i) {
    const uint32_t register_index = assignment->location_base + i;
    if (!loom_amdgpu_trans_result_window_entry_is_active(
            &window->entries[register_index])) {
      continue;
    }
    *out_origin = origins[register_index];
    return true;
  }
  return false;
}
