// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/channel_completion.h"

#include <string.h>

#include "loom/ops/channel/ops.h"

typedef struct loom_channel_completion_endpoint_t {
  // Protocol identity, independent of payload addresses.
  const loom_channel_identity_t* channel;
  // True for the producer's publication frontier; false for reclamation.
  bool producer;
  // Upper bound on admissions over reachable execution paths.
  uint32_t maximum_admissions;
} loom_channel_completion_endpoint_t;

typedef struct loom_channel_completion_record_t {
  // Endpoint ordinal; records are grouped by endpoint and FIFO admission order.
  uint32_t endpoint;
  // Outstanding owned value, or INVALID for a completed hole in the prefix.
  loom_value_id_t value;
} loom_channel_completion_record_t;

typedef struct loom_channel_completion_state_t {
  // Ordered live records and completed holes, excluding retired prefixes.
  loom_channel_completion_record_t* records;
  // Number of records in the snapshot.
  iree_host_size_t count;
} loom_channel_completion_state_t;

typedef struct loom_channel_completion_block_t {
  // Retained action interval in the canonical source plan.
  struct {
    // First action ordinal in the block.
    iree_host_size_t begin;
    // Exclusive end action ordinal.
    iree_host_size_t end;
  } actions;
  // First incoming state; later incoming edges must agree after renaming.
  loom_channel_completion_state_t input;
  // Whether an entry-reachable edge has established this input.
  bool reached;
} loom_channel_completion_block_t;

static void loom_channel_completion_require(
    loom_channel_completion_t* completion,
    loom_channel_completion_requirement_t requirement, const loom_op_t* op) {
  completion->requirement = requirement;
  completion->op = op;
}

static bool loom_channel_completion_apply(
    const loom_channel_plan_action_t* action, uint32_t endpoint,
    const loom_channel_completion_endpoint_t* endpoints,
    loom_channel_completion_state_t* state, uint32_t* credits,
    loom_channel_completion_t* completion) {
  const loom_op_t* op = action->op;
  if (loom_channel_reserve_isa(op) || loom_channel_acquire_isa(op)) {
    for (iree_host_size_t i = 0; i < state->count; ++i) {
      if (state->records[i].value == LOOM_VALUE_ID_INVALID &&
          !endpoints[state->records[i].endpoint].producer) {
        loom_channel_completion_require(
            completion, LOOM_CHANNEL_COMPLETION_REQUIREMENT_REUSE, op);
        return false;
      }
    }
    iree_host_size_t position = 0;
    while (position < state->count &&
           state->records[position].endpoint <= endpoint) {
      ++position;
    }
    memmove(&state->records[position + 1], &state->records[position],
            (state->count - position) * sizeof(*state->records));
    state->records[position] = (loom_channel_completion_record_t){
        .endpoint = endpoint, .value = loom_op_results(op)[0]};
    ++state->count;
    return true;
  }
  const loom_value_id_t value = loom_op_operands(op)[0];
  iree_host_size_t position = 0;
  while (position < state->count && state->records[position].value != value) {
    ++position;
  }
  if (position == state->count) {
    loom_channel_completion_require(
        completion, LOOM_CHANNEL_COMPLETION_REQUIREMENT_IDENTITY, op);
    return false;
  }
  if (loom_channel_wait_isa(op)) {
    // Every admitted read originates at acquire, so readiness is already
    // established and the same obligation remains live after borrowing.
    return true;
  }
  state->records[position].value = LOOM_VALUE_ID_INVALID;
  while (position && state->records[position - 1].endpoint == endpoint) {
    --position;
  }
  iree_host_size_t end = position;
  while (end < state->count && state->records[end].endpoint == endpoint &&
         state->records[end].value == LOOM_VALUE_ID_INVALID) {
    ++end;
  }
  *credits = (uint32_t)(end - position);
  memmove(&state->records[position], &state->records[end],
          (state->count - end) * sizeof(*state->records));
  state->count -= end - position;
  return true;
}

iree_status_t loom_channel_completion_analyze(
    const loom_channel_plan_t* plan, const loom_cfg_graph_t* graph,
    const uint64_t* block_execution_counts, iree_arena_allocator_t* arena,
    loom_channel_completion_t* out_completion) {
  *out_completion = (loom_channel_completion_t){0};
  if (!plan->action_count) {
    return iree_ok_status();
  }
  loom_channel_completion_action_t* actions = NULL;
  uint32_t* action_endpoints = NULL;
  loom_channel_completion_endpoint_t* endpoints = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, plan->action_count, sizeof(*actions), (void**)&actions));
  memset(actions, 0, plan->action_count * sizeof(*actions));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, plan->action_count,
                                                 sizeof(*action_endpoints),
                                                 (void**)&action_endpoints));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, plan->action_count, sizeof(*endpoints), (void**)&endpoints));
  uint32_t endpoint_count = 0;
  iree_host_size_t record_capacity = 0;
  for (iree_host_size_t i = 0; i < plan->action_count; ++i) {
    const loom_channel_plan_action_t* action = &plan->actions[i];
    const loom_op_t* op = action->op;
    if (!loom_channel_reserve_isa(op) && !loom_channel_acquire_isa(op) &&
        !loom_channel_publish_isa(op) && !loom_channel_release_isa(op) &&
        !loom_channel_wait_isa(op)) {
      loom_channel_completion_require(
          out_completion, LOOM_CHANNEL_COMPLETION_REQUIREMENT_ACTION, op);
      return iree_ok_status();
    }
    record_capacity +=
        loom_channel_reserve_isa(op) || loom_channel_acquire_isa(op);
    const bool producer =
        loom_channel_reserve_isa(op) || loom_channel_publish_isa(op);
    uint32_t endpoint = 0;
    while (endpoint < endpoint_count &&
           (endpoints[endpoint].channel != action->channel ||
            endpoints[endpoint].producer != producer)) {
      ++endpoint;
    }
    if (endpoint == endpoint_count) {
      endpoints[endpoint_count++] = (loom_channel_completion_endpoint_t){
          .channel = action->channel, .producer = producer};
    }
    action_endpoints[i] = endpoint;
  }
  loom_channel_completion_record_t* current_records = NULL;
  loom_channel_completion_record_t* edge_records = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, record_capacity,
                                                 sizeof(*current_records),
                                                 (void**)&current_records));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, record_capacity, sizeof(*edge_records), (void**)&edge_records));
  loom_channel_completion_block_t* blocks = NULL;
  uint16_t* worklist = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->block_count, sizeof(*blocks), (void**)&blocks));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->block_count, sizeof(*worklist), (void**)&worklist));
  iree_host_size_t action_index = 0;
  for (iree_host_size_t b = 0; b < graph->block_count; ++b) {
    blocks[b] =
        (loom_channel_completion_block_t){.actions.begin = action_index};
    while (action_index < plan->action_count &&
           plan->actions[action_index].op->parent_block ==
               graph->blocks[b].block) {
      const loom_op_t* op = plan->actions[action_index].op;
      if (graph->blocks[b].reachable &&
          (loom_channel_reserve_isa(op) || loom_channel_acquire_isa(op))) {
        uint32_t* bound =
            &endpoints[action_endpoints[action_index]].maximum_admissions;
        if (block_execution_counts) {
          const uint64_t count = block_execution_counts[b];
          *bound = count >= UINT32_MAX - *bound ? UINT32_MAX
                                                : *bound + (uint32_t)count;
        } else if (graph->blocks[b].component_is_cyclic) {
          *bound = UINT32_MAX;
        } else if (*bound != UINT32_MAX) {
          ++*bound;
        }
      }
      ++action_index;
    }
    blocks[b].actions.end = action_index;
  }
  iree_host_size_t read_position = 0, write_position = 1;
  worklist[0] = 0;
  blocks[0].reached = true;
  while (read_position < write_position) {
    const uint16_t index = worklist[read_position++];
    const loom_channel_completion_block_t* block = &blocks[index];
    loom_channel_completion_state_t current = {.records = current_records,
                                               .count = block->input.count};
    if (current.count) {
      memcpy(current_records, block->input.records,
             current.count * sizeof(*current_records));
    }
    for (iree_host_size_t i = block->actions.begin; i < block->actions.end;
         ++i) {
      if (!loom_channel_completion_apply(&plan->actions[i], action_endpoints[i],
                                         endpoints, &current,
                                         &actions[i].credits, out_completion)) {
        return iree_ok_status();
      }
    }
    const loom_cfg_edge_index_span_t successors =
        loom_cfg_graph_successor_edges(graph, index);
    if (!successors.count && current.count) {
      loom_channel_completion_require(
          out_completion, LOOM_CHANNEL_COMPLETION_REQUIREMENT_IDENTITY,
          graph->blocks[index].block->last_op);
      return iree_ok_status();
    }
    for (iree_host_size_t i = 0; i < successors.count; ++i) {
      const loom_cfg_edge_info_t* edge = &graph->edges[successors.values[i]];
      const loom_block_t* destination =
          graph->blocks[edge->target_block_index].block;
      if (current.count) {
        memcpy(edge_records, current_records,
               current.count * sizeof(*edge_records));
      }
      const loom_value_id_t* arguments = NULL;
      uint16_t argument_count = 0;
      if (destination->arg_count &&
          !loom_cfg_terminator_payload_for_successor(
              edge->terminator, destination, &arguments, &argument_count)) {
        loom_channel_completion_require(
            out_completion, LOOM_CHANNEL_COMPLETION_REQUIREMENT_CONTROL,
            edge->terminator);
        return iree_ok_status();
      }
      // Edge transfers are simultaneous, including rotating iterated reads.
      // Completed holes have no owned SSA name to rename.
      for (iree_host_size_t r = 0; r < current.count; ++r) {
        if (current_records[r].value == LOOM_VALUE_ID_INVALID) {
          continue;
        }
        for (uint16_t a = 0; a < argument_count; ++a) {
          if (arguments[a] == current_records[r].value) {
            edge_records[r].value = destination->arg_ids[a];
          }
        }
      }
      loom_channel_completion_block_t* target =
          &blocks[edge->target_block_index];
      if (target->reached) {
        if (target->input.count != current.count ||
            (current.count &&
             memcmp(target->input.records, edge_records,
                    current.count * sizeof(*edge_records)) != 0)) {
          loom_channel_completion_require(
              out_completion, LOOM_CHANNEL_COMPLETION_REQUIREMENT_CONTROL,
              edge->terminator);
          return iree_ok_status();
        }
      } else {
        IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
            arena, current.count, sizeof(*edge_records),
            (void**)&target->input.records));
        if (current.count) {
          memcpy(target->input.records, edge_records,
                 current.count * sizeof(*edge_records));
        }
        target->input.count = current.count;
        target->reached = true;
        worklist[write_position++] = edge->target_block_index;
      }
    }
  }
  for (iree_host_size_t i = 0; i < plan->action_count; ++i) {
    actions[i].maximum_admissions =
        endpoints[action_endpoints[i]].maximum_admissions;
  }
  out_completion->actions = actions;
  return iree_ok_status();
}
