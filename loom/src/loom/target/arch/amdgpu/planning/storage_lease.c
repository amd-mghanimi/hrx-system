// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/storage_lease.h"

#include "loom/target/arch/amdgpu/planning/structural_packet.h"
#include "loom/target/arch/amdgpu/planning/wait_counters.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/arch/amdgpu/target_info.h"
#include "loom/target/arch/amdgpu/target_info_defs.h"

typedef struct loom_amdgpu_storage_lease_query_t {
  // Target descriptor traits for the current node.
  loom_amdgpu_descriptor_traits_t descriptor_traits;
  // Downstream query sink.
  const loom_low_storage_lease_query_sink_t* sink;
} loom_amdgpu_storage_lease_query_t;

static iree_status_t loom_amdgpu_storage_lease_emit(
    void* user_data, const loom_low_storage_lease_event_t* event) {
  loom_amdgpu_storage_lease_query_t* query =
      (loom_amdgpu_storage_lease_query_t*)user_data;
  loom_low_storage_lease_event_t projected_event = *event;
  if (event->kind == LOOM_LOW_STORAGE_LEASE_SOURCE_READ &&
      event->release_class_id == LOOM_AMDGPU_WAIT_COUNTER_X) {
    projected_event.release_group_id =
        iree_any_bit_set(query->descriptor_traits,
                         LOOM_AMDGPU_DESCRIPTOR_TRAIT_VECTOR_MEMORY)
            ? LOOM_AMDGPU_XCNT_RELEASE_GROUP_VMEM
            : LOOM_AMDGPU_XCNT_RELEASE_GROUP_SMEM;
  }
  return query->sink->emit_lease(query->sink->user_data, &projected_event);
}

static iree_status_t loom_amdgpu_storage_progress_bound_emit(
    const loom_low_schedule_table_t* schedule,
    const loom_low_schedule_node_t* node,
    loom_amdgpu_descriptor_traits_t descriptor_traits,
    loom_low_storage_progress_bound_emit_fn_t emit, void* emit_user_data) {
  loom_low_storage_progress_bound_event_t event = {
      .release_class_id = LOOM_AMDGPU_WAIT_COUNTER_X,
      .remaining_count = 0,
  };
  if (iree_any_bit_set(descriptor_traits,
                       LOOM_AMDGPU_DESCRIPTOR_TRAIT_XCNT_IMPLICIT_DRAIN) ||
      (node->kind == LOOM_LOW_SCHEDULE_NODE_TERMINATOR &&
       loom_amdgpu_structural_packet_control_transfer_count(schedule, node) !=
           0)) {
    event.kind = LOOM_LOW_STORAGE_PROGRESS_BOUND_PACKET;
    event.release_group_id = LOOM_LOW_STORAGE_LEASE_RELEASE_GROUP_ALL;
    return emit(emit_user_data, &event);
  }
  if (iree_any_bit_set(descriptor_traits,
                       LOOM_AMDGPU_DESCRIPTOR_TRAIT_WRITES_EXEC)) {
    event.kind = LOOM_LOW_STORAGE_PROGRESS_BOUND_REQUIRED;
    event.release_group_id = LOOM_AMDGPU_XCNT_RELEASE_GROUP_VMEM;
    return emit(emit_user_data, &event);
  }
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_storage_lease_query(
    void* user_data, const loom_low_schedule_table_t* schedule,
    const loom_low_schedule_node_t* node,
    const loom_low_storage_lease_query_sink_t* sink) {
  if (schedule == NULL || node == NULL ||
      schedule->target.descriptor_set == NULL) {
    return iree_ok_status();
  }
  if (node->descriptor == NULL &&
      node->kind != LOOM_LOW_SCHEDULE_NODE_TERMINATOR) {
    return iree_ok_status();
  }
  const loom_low_descriptor_set_t* descriptor_set =
      schedule->target.descriptor_set;
  if (descriptor_set->target_stable_id != LOOM_AMDGPU_TARGET_STABLE_ID) {
    return iree_ok_status();
  }
  const loom_amdgpu_descriptor_set_info_t* descriptor_set_info =
      loom_amdgpu_target_info_descriptor_set_at(
          descriptor_set->descriptor_set_ordinal);
  const bool supports_xcnt =
      descriptor_set_info != NULL &&
      loom_amdgpu_descriptor_set_info_has_flags(
          descriptor_set_info,
          LOOM_AMDGPU_DESCRIPTOR_SET_INFO_FLAG_XCNT_SOURCE_RETENTION);
  if (!supports_xcnt) {
    return loom_low_storage_lease_query_descriptor_rows(user_data, schedule,
                                                        node, sink);
  }
  const loom_amdgpu_descriptor_traits_t descriptor_traits =
      node->descriptor == NULL
          ? 0
          : loom_amdgpu_descriptor_traits(descriptor_set, node->descriptor);
  loom_amdgpu_storage_lease_query_t query = {
      .descriptor_traits = descriptor_traits,
      .sink = sink,
  };
  const loom_low_storage_lease_query_sink_t projected_sink = {
      .user_data = &query,
      .emit_lease = loom_amdgpu_storage_lease_emit,
      .emit_progress_bound = sink->emit_progress_bound,
  };
  IREE_RETURN_IF_ERROR(loom_low_storage_lease_query_descriptor_rows(
      user_data, schedule, node, &projected_sink));
  return loom_amdgpu_storage_progress_bound_emit(
      schedule, node, descriptor_traits, sink->emit_progress_bound,
      sink->user_data);
}

void loom_amdgpu_storage_lease_provider(
    loom_low_storage_lease_provider_t* out_provider) {
  IREE_ASSERT_ARGUMENT(out_provider);
  *out_provider = (loom_low_storage_lease_provider_t){
      .query = loom_amdgpu_storage_lease_query,
  };
}
