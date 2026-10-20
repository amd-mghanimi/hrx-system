// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_REPLAY_RECORDER_BARRIER_H_
#define IREE_HAL_REPLAY_RECORDER_BARRIER_H_

#include "iree/hal/api.h"
#include "iree/hal/replay/recorder.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Rewritten native descriptors and serialized boundary records for one call.
// The caller's buffers and memory barrier arrays remain borrowed until return.
// One allocation holds all rewritten arrays and the serialized extension.
typedef struct iree_hal_replay_recorder_barriers_t {
  // Underlying queue operand referencing |lists|.
  iree_hal_queue_barriers_t base;
  // Before and after descriptor arrays, respectively.
  iree_hal_barrier_list_t lists[2];
  // Serialized barrier lists followed by their footer, or empty for defaults.
  iree_const_byte_span_t payload;
  // Whether serialized barriers append transition recipe payloads.
  bool has_transition_recipes;
  // Whether all buffer references belong to this recording session.
  bool can_record;
  // Owned storage for rewritten descriptors and serialized bytes.
  void* allocation;
} iree_hal_replay_recorder_barriers_t;

// Captures the supplied descriptors and rewrites recording buffer proxies to
// their underlying ranges. Failure leaves |out_storage| empty.
iree_status_t iree_hal_replay_recorder_barriers_initialize(
    iree_hal_replay_recorder_t* recorder,
    const iree_hal_queue_barriers_t* barriers, iree_allocator_t host_allocator,
    iree_hal_replay_recorder_barriers_t* out_storage);

// Frees storage after the underlying queue has captured the descriptors and
// the recorder has serialized the operation.
void iree_hal_replay_recorder_barriers_deinitialize(
    iree_allocator_t host_allocator,
    iree_hal_replay_recorder_barriers_t* storage);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_REPLAY_RECORDER_BARRIER_H_
