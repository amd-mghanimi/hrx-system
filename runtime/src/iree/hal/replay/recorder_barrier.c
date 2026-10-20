// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/replay/recorder_barrier.h"

#include <string.h>

#include "iree/hal/replay/recorder_buffer.h"

static bool iree_hal_replay_recorder_barriers_accumulate_size(
    iree_host_size_t count, iree_host_size_t element_size,
    iree_host_size_t* total_size) {
  iree_host_size_t size = 0;
  return iree_host_size_checked_mul(count, element_size, &size) &&
         iree_host_size_checked_add(*total_size, size, total_size);
}

static iree_hal_replay_memory_transition_operation_payload_t
iree_hal_replay_recorder_memory_transition_operation_payload(
    const iree_hal_memory_transition_recipe_info_t* operation) {
  return (iree_hal_replay_memory_transition_operation_payload_t){
      .range_granularity = operation->range_granularity,
      .kind = operation->kind,
      .executor = operation->executor,
      .operation = operation->operation,
      .host_instruction = operation->host.instruction,
      .host_fence_before = operation->host.fence_before,
      .host_fence_after = operation->host.fence_after,
  };
}

void iree_hal_replay_recorder_barriers_deinitialize(
    iree_allocator_t host_allocator,
    iree_hal_replay_recorder_barriers_t* storage) {
  iree_allocator_free(host_allocator, storage->allocation);
  memset(storage, 0, sizeof(*storage));
}

iree_status_t iree_hal_replay_recorder_barriers_initialize(
    iree_hal_replay_recorder_t* recorder,
    const iree_hal_queue_barriers_t* barriers, iree_allocator_t host_allocator,
    iree_hal_replay_recorder_barriers_t* out_storage) {
  memset(out_storage, 0, sizeof(*out_storage));
  out_storage->can_record = true;
  if (!barriers || (!barriers->before && !barriers->after)) {
    return iree_ok_status();
  }
  const iree_hal_barrier_list_t* lists[2] = {barriers->before, barriers->after};
  iree_host_size_t native_size = 0;
  iree_host_size_t wire_size = sizeof(iree_hal_replay_queue_barriers_footer_t);
  iree_host_size_t buffer_barrier_count = 0;
  iree_host_size_t transition_operation_count = 0;
  bool has_transition_recipes = false;
  for (iree_host_size_t boundary = 0; boundary < 2; ++boundary) {
    const iree_hal_barrier_list_t* list = lists[boundary];
    if (!list) {
      continue;
    }
    if (!iree_hal_replay_recorder_barriers_accumulate_size(
            list->count, sizeof(iree_hal_barrier_t), &native_size) ||
        !iree_hal_replay_recorder_barriers_accumulate_size(
            list->count,
            sizeof(iree_hal_replay_command_buffer_execution_barrier_payload_t),
            &wire_size)) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "replay barrier list size overflow");
    }
    for (iree_host_size_t i = 0; i < list->count; ++i) {
      const iree_hal_barrier_t* barrier = &list->values[i];
      if (!iree_host_size_checked_add(buffer_barrier_count,
                                      barrier->buffer_barrier_count,
                                      &buffer_barrier_count)) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "replay buffer barrier count overflow");
      }
      for (iree_host_size_t j = 0; j < barrier->buffer_barrier_count; ++j) {
        const iree_hal_memory_transition_recipe_t* recipe =
            barrier->buffer_barriers[j].recipe;
        if (!recipe) {
          continue;
        }
        has_transition_recipes = true;
        if (!iree_host_size_checked_add(transition_operation_count,
                                        recipe->operation_count,
                                        &transition_operation_count)) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "replay transition operation count overflow");
        }
      }
      if (!iree_hal_replay_recorder_barriers_accumulate_size(
              barrier->buffer_barrier_count, sizeof(iree_hal_buffer_barrier_t),
              &native_size) ||
          !iree_hal_replay_recorder_barriers_accumulate_size(
              barrier->memory_barrier_count,
              sizeof(iree_hal_replay_memory_barrier_payload_t), &wire_size) ||
          !iree_hal_replay_recorder_barriers_accumulate_size(
              barrier->buffer_barrier_count,
              sizeof(iree_hal_replay_buffer_barrier_payload_t), &wire_size)) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "replay barrier storage size overflow");
      }
    }
  }
  if (has_transition_recipes &&
      (!iree_hal_replay_recorder_barriers_accumulate_size(
           buffer_barrier_count,
           sizeof(iree_hal_replay_memory_transition_recipe_payload_t),
           &wire_size) ||
       !iree_hal_replay_recorder_barriers_accumulate_size(
           transition_operation_count,
           sizeof(iree_hal_replay_memory_transition_operation_payload_t),
           &wire_size))) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "replay transition recipe storage overflow");
  }
  iree_host_size_t total_size = 0;
  if (!iree_host_size_checked_add(native_size, wire_size, &total_size)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "replay barrier allocation size overflow");
  }
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, total_size,
                                             &out_storage->allocation));
  uint8_t* native = out_storage->allocation;
  uint8_t* wire = native + native_size;
  out_storage->payload = iree_make_const_byte_span(wire, wire_size);
  out_storage->has_transition_recipes = has_transition_recipes;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t boundary = 0; boundary < 2 && iree_status_is_ok(status);
       ++boundary) {
    const iree_hal_barrier_list_t* list = lists[boundary];
    if (!list) {
      continue;
    }
    iree_hal_barrier_t* values = (iree_hal_barrier_t*)native;
    native += list->count * sizeof(*values);
    out_storage->lists[boundary] =
        (iree_hal_barrier_list_t){.count = list->count, .values = values};
    for (iree_host_size_t i = 0; i < list->count && iree_status_is_ok(status);
         ++i) {
      const iree_hal_barrier_t* barrier = &list->values[i];
      values[i] = *barrier;
      iree_hal_replay_command_buffer_execution_barrier_payload_t header = {
          .source_stage_mask = barrier->source_stage_mask,
          .target_stage_mask = barrier->target_stage_mask,
          .flags = iree_hal_barrier_resolve_flags(barrier),
          .memory_barrier_count = barrier->memory_barrier_count,
          .buffer_barrier_count = barrier->buffer_barrier_count,
      };
      memcpy(wire, &header, sizeof(header));
      wire += sizeof(header);
      for (iree_host_size_t j = 0; j < barrier->memory_barrier_count; ++j) {
        iree_hal_replay_memory_barrier_payload_t payload = {
            .source_scope = barrier->memory_barriers[j].source_scope,
            .target_scope = barrier->memory_barriers[j].target_scope,
        };
        memcpy(wire, &payload, sizeof(payload));
        wire += sizeof(payload);
      }
      iree_hal_buffer_barrier_t* buffers = (iree_hal_buffer_barrier_t*)native;
      native += barrier->buffer_barrier_count * sizeof(*buffers);
      values[i].buffer_barriers = buffers;
      for (iree_host_size_t j = 0;
           j < barrier->buffer_barrier_count && iree_status_is_ok(status);
           ++j) {
        buffers[j] = barrier->buffer_barriers[j];
        iree_hal_replay_buffer_barrier_payload_t payload = {
            .source_scope = buffers[j].source_scope,
            .target_scope = buffers[j].target_scope,
        };
        iree_hal_replay_recorder_buffer_ref_make_payload(buffers[j].buffer_ref,
                                                         &payload.buffer_ref);
        if (buffers[j].buffer_ref.buffer) {
          payload.buffer_ref.buffer_id =
              iree_hal_replay_recorder_find_buffer_id(
                  recorder, buffers[j].buffer_ref.buffer);
          out_storage->can_record &=
              payload.buffer_ref.buffer_id != IREE_HAL_REPLAY_OBJECT_ID_NONE;
        }
        memcpy(wire, &payload, sizeof(payload));
        wire += sizeof(payload);
        status = iree_hal_replay_recorder_buffer_ref_unwrap_for_call(
            &buffers[j].buffer_ref);
      }
      if (out_storage->has_transition_recipes) {
        for (iree_host_size_t j = 0; j < barrier->buffer_barrier_count; ++j) {
          const iree_hal_memory_transition_recipe_t* recipe = buffers[j].recipe;
          const iree_hal_replay_memory_transition_recipe_payload_t payload = {
              .effects = recipe ? recipe->effects.bits : 0,
              .operation_count = recipe ? recipe->operation_count : 0,
          };
          memcpy(wire, &payload, sizeof(payload));
          wire += sizeof(payload);
        }
        for (iree_host_size_t j = 0; j < barrier->buffer_barrier_count; ++j) {
          const iree_hal_memory_transition_recipe_t* recipe = buffers[j].recipe;
          if (!recipe) {
            continue;
          }
          for (uint32_t k = 0; k < recipe->operation_count; ++k) {
            const iree_hal_replay_memory_transition_operation_payload_t payload =
                iree_hal_replay_recorder_memory_transition_operation_payload(
                    &recipe->operations[k]);
            memcpy(wire, &payload, sizeof(payload));
            wire += sizeof(payload);
          }
        }
      }
    }
  }
  if (iree_status_is_ok(status)) {
    iree_hal_replay_queue_barriers_footer_t footer = {
        .payload_length = wire_size - sizeof(footer),
        .before_count = lists[0] ? lists[0]->count : UINT64_MAX,
        .after_count = lists[1] ? lists[1]->count : UINT64_MAX,
    };
    memcpy(wire, &footer, sizeof(footer));
    out_storage->base.before = lists[0] ? &out_storage->lists[0] : NULL;
    out_storage->base.after = lists[1] ? &out_storage->lists[1] : NULL;
  } else {
    iree_hal_replay_recorder_barriers_deinitialize(host_allocator, out_storage);
  }
  return status;
}
