// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_TRANS_RESULT_WINDOW_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_TRANS_RESULT_WINDOW_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/allocation.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef enum loom_amdgpu_trans_result_window_flag_bits_e {
  // Retain the caller-supplied origin for each active physical VGPR.
  LOOM_AMDGPU_TRANS_RESULT_WINDOW_FLAG_TRACK_ORIGINS = 1u << 0,
} loom_amdgpu_trans_result_window_flag_bits_t;
typedef uint8_t loom_amdgpu_trans_result_window_flags_t;

typedef enum loom_amdgpu_trans_result_packet_flag_bits_e {
  // Packet occupies one vector ALU interval in the hazard window.
  LOOM_AMDGPU_TRANS_RESULT_PACKET_FLAG_VECTOR_ALU = 1u << 0,
  // Packet occupies one transcendental interval in the hazard window.
  LOOM_AMDGPU_TRANS_RESULT_PACKET_FLAG_TRANSCENDENTAL = 1u << 1,
} loom_amdgpu_trans_result_packet_flag_bits_t;
typedef uint8_t loom_amdgpu_trans_result_packet_flags_t;

typedef struct loom_amdgpu_trans_result_window_entry_t
    loom_amdgpu_trans_result_window_entry_t;

// Active GFX11 transcendental-result hazard windows by physical VGPR.
typedef struct loom_amdgpu_trans_result_window_t {
  // Packed per-register state followed by active indices and optional origins.
  loom_amdgpu_trans_result_window_entry_t* entries;
  // Number of physical VGPR entries.
  uint32_t register_count;
  // Number of physical VGPR indices in the packed active list.
  uint32_t active_register_count;
  // Initialization features controlling optional packed state.
  loom_amdgpu_trans_result_window_flags_t flags;
} loom_amdgpu_trans_result_window_t;

// Initializes |out_window| for |register_count| physical VGPRs. The window
// remains empty until an assignment is recorded.
iree_status_t loom_amdgpu_trans_result_window_initialize(
    iree_host_size_t register_count,
    loom_amdgpu_trans_result_window_flags_t flags,
    iree_arena_allocator_t* arena,
    loom_amdgpu_trans_result_window_t* out_window);

// Returns true when storage was initialized for at least one physical VGPR.
bool loom_amdgpu_trans_result_window_is_initialized(
    const loom_amdgpu_trans_result_window_t* window);

// Returns true when at least one physical VGPR has an active hazard window.
bool loom_amdgpu_trans_result_window_has_active(
    const loom_amdgpu_trans_result_window_t* window);

// Clears all active hazard windows while retaining allocated storage.
void loom_amdgpu_trans_result_window_clear(
    loom_amdgpu_trans_result_window_t* window);

// Advances active windows by one native packet and expires windows beyond the
// architecture-defined vector ALU or transcendental interval.
void loom_amdgpu_trans_result_window_advance(
    loom_amdgpu_trans_result_window_t* window,
    loom_amdgpu_trans_result_packet_flags_t packet_flags);

// Clears active windows overlapping a physical-VGPR |assignment|. Other
// allocation locations and register classes are ignored.
void loom_amdgpu_trans_result_window_clear_assignment(
    loom_amdgpu_trans_result_window_t* window,
    const loom_low_allocation_assignment_t* assignment);

// Starts fresh hazard windows for a physical-VGPR |assignment|. |origin| is
// retained only when TRACK_ORIGINS was selected during initialization.
void loom_amdgpu_trans_result_window_record_assignment(
    loom_amdgpu_trans_result_window_t* window,
    const loom_low_allocation_assignment_t* assignment, uint32_t origin);

// Returns true when a physical-VGPR |assignment| overlaps an active window and
// stores its origin in |out_origin|. Requires TRACK_ORIGINS initialization.
bool loom_amdgpu_trans_result_window_query_assignment_origin(
    const loom_amdgpu_trans_result_window_t* window,
    const loom_low_allocation_assignment_t* assignment, uint32_t* out_origin);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_TRANS_RESULT_WINDOW_H_
