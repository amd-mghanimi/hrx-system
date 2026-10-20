// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/schedule/memory_completion.h"

#include <string.h>

#include "iree/base/internal/math.h"

enum {
  LOOM_LOW_SCHEDULE_MEMORY_COMPLETION_BITS_PER_WORD = 64,
};

typedef struct loom_low_schedule_refined_memory_effect_t {
  // Effect-use index in final scheduled order.
  uint32_t effect_use;
  // Stable source-derived access summary owned by the memory-access map.
  const loom_low_memory_access_summary_t* summary;
} loom_low_schedule_refined_memory_effect_t;

static bool loom_low_schedule_memory_summary_can_refine_aliases(
    const loom_low_memory_access_summary_t* summary) {
  const loom_low_memory_access_precision_flags_t alias_precision =
      LOOM_LOW_MEMORY_ACCESS_PRECISION_ROOT |
      LOOM_LOW_MEMORY_ACCESS_PRECISION_GROUP |
      LOOM_LOW_MEMORY_ACCESS_PRECISION_INTERVAL |
      LOOM_LOW_MEMORY_ACCESS_PRECISION_STRIDED_INTERVAL;
  return summary->relative_interval != NULL ||
         iree_any_bit_set(summary->precision_flags, alias_precision);
}

static const loom_low_memory_access_summary_t*
loom_low_schedule_refined_memory_summary(
    const loom_low_schedule_build_state_t* state,
    const loom_low_schedule_effect_use_t* effect) {
  if (!iree_any_bit_set(effect->effect_flags,
                        LOOM_LOW_EFFECT_FLAG_DEPENDENCY) ||
      iree_any_bit_set(effect->effect_flags, LOOM_LOW_EFFECT_FLAG_ORDERED) ||
      (effect->kind != LOOM_LOW_EFFECT_KIND_READ &&
       effect->kind != LOOM_LOW_EFFECT_KIND_WRITE)) {
    return NULL;
  }
  const loom_low_schedule_node_t* node = &state->nodes[effect->node_index];
  if (iree_any_bit_set(node->op->instance_flags,
                       LOOM_MEMORY_ACCESS_FLAG_VOLATILE)) {
    return NULL;
  }
  const loom_low_memory_access_summary_t* summary =
      loom_low_memory_access_map_lookup(state->memory_accesses, node->op,
                                        effect->effect_ordinal);
  return summary != NULL &&
                 loom_low_schedule_memory_summary_can_refine_aliases(summary)
             ? summary
             : NULL;
}

static iree_status_t loom_low_schedule_append_memory_completion_edge(
    loom_low_schedule_build_state_t* state,
    loom_low_schedule_memory_completion_edge_t** edges,
    iree_host_size_t* edge_count, iree_host_size_t* edge_capacity,
    uint32_t producer_effect_use, uint32_t consumer_effect_use) {
  if (*edge_count == *edge_capacity) {
    if (*edge_count == IREE_HOST_SIZE_MAX) {
      return iree_make_status(
          IREE_STATUS_RESOURCE_EXHAUSTED,
          "low schedule memory-completion edge count exceeds host size");
    }
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        state->scratch_arena, *edge_count, *edge_count + 1, sizeof(**edges),
        edge_capacity, (void**)edges));
  }
  (*edges)[(*edge_count)++] = (loom_low_schedule_memory_completion_edge_t){
      .producer_effect_use = producer_effect_use,
      .consumer_effect_use = consumer_effect_use,
  };
  return iree_ok_status();
}

static iree_status_t loom_low_schedule_consume_memory_frontier(
    loom_low_schedule_build_state_t* state,
    const loom_low_schedule_refined_memory_effect_t* refined_effects,
    const loom_low_schedule_effect_use_t* consumer,
    const loom_low_memory_access_summary_t* consumer_summary,
    iree_host_size_t frontier_word_count, uint64_t* frontier_words,
    loom_low_schedule_memory_completion_edge_t** edges,
    iree_host_size_t* edge_count, iree_host_size_t* edge_capacity) {
  const uint32_t consumer_effect_use =
      (uint32_t)(consumer - state->effect_uses);
  for (iree_host_size_t word_index = 0; word_index < frontier_word_count;
       ++word_index) {
    uint64_t bits = frontier_words[word_index];
    while (bits != 0) {
      const uint32_t bit = (uint32_t)iree_math_count_trailing_zeros_u64(bits);
      const iree_host_size_t refined_ordinal = word_index * 64 + bit;
      const loom_low_schedule_refined_memory_effect_t* producer =
          &refined_effects[refined_ordinal];
      if (!loom_low_memory_access_summaries_may_alias(
              producer->summary, consumer_summary,
              LOOM_LOW_MEMORY_COMPARISON_SAME_ACYCLIC_INVOCATION)) {
        bits &= bits - 1;
        continue;
      }
      const loom_low_schedule_effect_use_t* producer_effect =
          &state->effect_uses[producer->effect_use];
      if (producer_effect->block_index != consumer->block_index) {
        IREE_RETURN_IF_ERROR(loom_low_schedule_append_memory_completion_edge(
            state, edges, edge_count, edge_capacity, producer->effect_use,
            consumer_effect_use));
      }
      frontier_words[word_index] &= ~(UINT64_C(1) << bit);
      bits &= bits - 1;
    }
  }
  return iree_ok_status();
}

static void loom_low_schedule_union_forward_memory_frontiers(
    const loom_cfg_graph_t* graph, uint16_t block_index,
    const iree_host_size_t* frontier_row_offsets, uint64_t* read_frontiers,
    uint64_t* write_frontiers) {
  const iree_host_size_t target_offset = frontier_row_offsets[block_index];
  uint64_t* target_reads = read_frontiers + target_offset;
  uint64_t* target_writes = write_frontiers + target_offset;
  const loom_cfg_edge_index_span_t incoming_edges =
      loom_cfg_graph_predecessor_edges(graph, block_index);
  for (iree_host_size_t i = 0; i < incoming_edges.count; ++i) {
    const loom_cfg_edge_info_t* edge =
        loom_cfg_graph_edge(graph, incoming_edges.values[i]);
    if (edge->source_block_index >= block_index) {
      continue;
    }
    const iree_host_size_t source_offset =
        frontier_row_offsets[edge->source_block_index];
    const iree_host_size_t source_word_count =
        frontier_row_offsets[edge->source_block_index + 1] - source_offset;
    const uint64_t* source_reads = read_frontiers + source_offset;
    const uint64_t* source_writes = write_frontiers + source_offset;
    for (iree_host_size_t word_index = 0; word_index < source_word_count;
         ++word_index) {
      target_reads[word_index] |= source_reads[word_index];
      target_writes[word_index] |= source_writes[word_index];
    }
  }
}

iree_status_t loom_low_schedule_build_acyclic_memory_completions(
    loom_low_schedule_build_state_t* state) {
  if (state->memory_accesses == NULL || state->effect_use_count == 0 ||
      state->cfg_graph->edge_count == 0) {
    return iree_ok_status();
  }
  if (state->effect_use_count > UINT32_MAX) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "low schedule effect-use count exceeds memory-completion index range");
  }

  iree_host_size_t refined_effect_count = 0;
  for (iree_host_size_t i = 0; i < state->effect_use_count; ++i) {
    loom_low_schedule_effect_use_t* effect = &state->effect_uses[i];
    if (loom_low_schedule_refined_memory_summary(state, effect) == NULL) {
      continue;
    }
    effect->flags |= LOOM_LOW_SCHEDULE_EFFECT_USE_FLAG_REFINED_MEMORY;
    ++refined_effect_count;
  }
  if (refined_effect_count == 0) {
    return iree_ok_status();
  }

  loom_low_schedule_refined_memory_effect_t* refined_effects = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      state->scratch_arena, refined_effect_count, sizeof(*refined_effects),
      (void**)&refined_effects));
  iree_host_size_t refined_ordinal = 0;
  for (iree_host_size_t i = 0; i < state->effect_use_count; ++i) {
    const loom_low_schedule_effect_use_t* effect = &state->effect_uses[i];
    if (!iree_any_bit_set(effect->flags,
                          LOOM_LOW_SCHEDULE_EFFECT_USE_FLAG_REFINED_MEMORY)) {
      continue;
    }
    refined_effects[refined_ordinal++] =
        (loom_low_schedule_refined_memory_effect_t){
            .effect_use = (uint32_t)i,
            .summary = loom_low_memory_access_map_lookup(
                state->memory_accesses, state->nodes[effect->node_index].op,
                effect->effect_ordinal),
        };
  }
  IREE_ASSERT_EQ(refined_ordinal, refined_effect_count);

  iree_host_size_t* frontier_row_offsets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      state->scratch_arena, state->cfg_graph->block_count + 1,
      sizeof(*frontier_row_offsets), (void**)&frontier_row_offsets));
  frontier_row_offsets[0] = 0;
  iree_host_size_t effect_use_index = 0;
  refined_ordinal = 0;
  for (uint16_t block_index = 0; block_index < state->cfg_graph->block_count;
       ++block_index) {
    while (effect_use_index < state->effect_use_count &&
           state->effect_uses[effect_use_index].block_index == block_index) {
      refined_ordinal +=
          iree_any_bit_set(state->effect_uses[effect_use_index].flags,
                           LOOM_LOW_SCHEDULE_EFFECT_USE_FLAG_REFINED_MEMORY);
      ++effect_use_index;
    }
    const iree_host_size_t row_word_count = iree_host_size_ceil_div(
        refined_ordinal, LOOM_LOW_SCHEDULE_MEMORY_COMPLETION_BITS_PER_WORD);
    if (!iree_host_size_checked_add(frontier_row_offsets[block_index],
                                    row_word_count,
                                    &frontier_row_offsets[block_index + 1])) {
      return iree_make_status(
          IREE_STATUS_RESOURCE_EXHAUSTED,
          "low schedule memory-completion frontier exceeds host size");
    }
  }
  IREE_ASSERT_EQ(effect_use_index, state->effect_use_count);
  IREE_ASSERT_EQ(refined_ordinal, refined_effect_count);

  const iree_host_size_t frontier_state_count =
      frontier_row_offsets[state->cfg_graph->block_count];
  uint64_t* read_frontiers = NULL;
  uint64_t* write_frontiers = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      state->scratch_arena, frontier_state_count, sizeof(*read_frontiers),
      (void**)&read_frontiers));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      state->scratch_arena, frontier_state_count, sizeof(*write_frontiers),
      (void**)&write_frontiers));
  memset(read_frontiers, 0, frontier_state_count * sizeof(*read_frontiers));
  memset(write_frontiers, 0, frontier_state_count * sizeof(*write_frontiers));

  loom_low_schedule_memory_completion_edge_t* scratch_edges = NULL;
  iree_host_size_t scratch_edge_count = 0;
  iree_host_size_t scratch_edge_capacity = 0;
  effect_use_index = 0;
  refined_ordinal = 0;
  for (uint16_t block_index = 0; block_index < state->cfg_graph->block_count;
       ++block_index) {
    IREE_ASSERT(effect_use_index == state->effect_use_count ||
                state->effect_uses[effect_use_index].block_index >=
                    block_index);
    const iree_host_size_t block_effect_start = effect_use_index;
    while (effect_use_index < state->effect_use_count &&
           state->effect_uses[effect_use_index].block_index == block_index) {
      ++effect_use_index;
    }
    if (!loom_cfg_graph_block_is_reachable(state->cfg_graph, block_index)) {
      for (iree_host_size_t i = block_effect_start; i < effect_use_index; ++i) {
        refined_ordinal +=
            iree_any_bit_set(state->effect_uses[i].flags,
                             LOOM_LOW_SCHEDULE_EFFECT_USE_FLAG_REFINED_MEMORY);
      }
      continue;
    }

    loom_low_schedule_union_forward_memory_frontiers(
        state->cfg_graph, block_index, frontier_row_offsets, read_frontiers,
        write_frontiers);
    const iree_host_size_t block_frontier_offset =
        frontier_row_offsets[block_index];
    uint64_t* block_reads = read_frontiers + block_frontier_offset;
    uint64_t* block_writes = write_frontiers + block_frontier_offset;
    for (iree_host_size_t node_effect_start = block_effect_start;
         node_effect_start < effect_use_index;) {
      const uint32_t node_index =
          state->effect_uses[node_effect_start].node_index;
      iree_host_size_t node_effect_end = node_effect_start + 1;
      while (node_effect_end < effect_use_index &&
             state->effect_uses[node_effect_end].node_index == node_index) {
        ++node_effect_end;
      }

      const iree_host_size_t active_node_word_count = iree_host_size_ceil_div(
          refined_ordinal, LOOM_LOW_SCHEDULE_MEMORY_COMPLETION_BITS_PER_WORD);
      iree_host_size_t node_refined_ordinal = refined_ordinal;
      for (iree_host_size_t i = node_effect_start; i < node_effect_end; ++i) {
        const loom_low_schedule_effect_use_t* effect = &state->effect_uses[i];
        if (!iree_any_bit_set(
                effect->flags,
                LOOM_LOW_SCHEDULE_EFFECT_USE_FLAG_REFINED_MEMORY)) {
          continue;
        }
        const loom_low_memory_access_summary_t* summary =
            refined_effects[node_refined_ordinal++].summary;
        uint64_t* opposite_frontier = effect->kind == LOOM_LOW_EFFECT_KIND_READ
                                          ? block_writes
                                          : block_reads;
        IREE_RETURN_IF_ERROR(loom_low_schedule_consume_memory_frontier(
            state, refined_effects, effect, summary, active_node_word_count,
            opposite_frontier, &scratch_edges, &scratch_edge_count,
            &scratch_edge_capacity));
      }

      node_refined_ordinal = refined_ordinal;
      for (iree_host_size_t i = node_effect_start; i < node_effect_end; ++i) {
        const loom_low_schedule_effect_use_t* effect = &state->effect_uses[i];
        if (!iree_any_bit_set(
                effect->flags,
                LOOM_LOW_SCHEDULE_EFFECT_USE_FLAG_REFINED_MEMORY)) {
          continue;
        }
        uint64_t* frontier = effect->kind == LOOM_LOW_EFFECT_KIND_READ
                                 ? block_reads
                                 : block_writes;
        const iree_host_size_t word_index = node_refined_ordinal / 64;
        const uint32_t bit = (uint32_t)(node_refined_ordinal % 64);
        frontier[word_index] |= UINT64_C(1) << bit;
        ++node_refined_ordinal;
      }
      refined_ordinal = node_refined_ordinal;
      node_effect_start = node_effect_end;
    }
  }
  IREE_ASSERT_EQ(effect_use_index, state->effect_use_count);
  IREE_ASSERT_EQ(refined_ordinal, refined_effect_count);

  if (scratch_edge_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(state->arena, scratch_edge_count,
                                  sizeof(*state->memory_completion_edges),
                                  (void**)&state->memory_completion_edges));
    memcpy(state->memory_completion_edges, scratch_edges,
           scratch_edge_count * sizeof(*state->memory_completion_edges));
  }
  state->memory_completion_edge_count = scratch_edge_count;
  return iree_ok_status();
}
