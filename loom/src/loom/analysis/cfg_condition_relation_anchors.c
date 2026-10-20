// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/cfg_condition_relation_anchors.h"

#include <string.h>

#include "loom/util/adaptive_sort.h"

// One construction association from a local semantic anchor to an authored
// relation operand.
typedef struct loom_cfg_condition_relation_anchor_pair_t {
  // Local value ordinal used by indexed queries.
  loom_value_ordinal_t anchor;

  // Dense authored operand whose relation rows the anchor may retrieve.
  loom_cfg_condition_operand_t operand;
} loom_cfg_condition_relation_anchor_pair_t;

// One sparse retained anchor and its authored relation operands.
typedef struct loom_cfg_condition_relation_anchor_entry_t {
  // Canonical local value ordinal of the semantic anchor.
  loom_value_ordinal_t anchor;

  // Set of dense authored relation operands associated with the anchor.
  loom_condition_relation_set_id_t operands;
} loom_cfg_condition_relation_anchor_entry_t;

static_assert(sizeof(loom_cfg_condition_relation_anchor_entry_t) == 8,
              "condition relation anchor entries must remain compact");

struct loom_cfg_condition_relation_anchor_builder_t {
  // Scratch arena owning every construction allocation.
  iree_arena_allocator_t* arena;

  // Unsorted and potentially duplicate anchor associations.
  loom_cfg_condition_relation_anchor_pair_t* pairs;

  // Number of populated entries in pairs.
  iree_host_size_t pair_count;

  // Allocated entry count in pairs.
  iree_host_size_t pair_capacity;

  // Sorted unique anchors with construction set roots.
  loom_cfg_condition_relation_anchor_entry_t* entries;

  // Number of populated entries in entries.
  uint32_t entry_count;
};

static bool loom_cfg_condition_relation_anchor_pair_less(
    const loom_cfg_condition_relation_anchor_pair_t* left,
    const loom_cfg_condition_relation_anchor_pair_t* right) {
  return left->anchor < right->anchor ||
         (left->anchor == right->anchor && left->operand < right->operand);
}

LOOM_DEFINE_ADAPTIVE_SORT(loom_cfg_condition_relation_sort_anchor_pairs,
                          loom_cfg_condition_relation_anchor_pair_t,
                          loom_cfg_condition_relation_anchor_pair_less)

static iree_status_t loom_cfg_condition_relation_anchor_builder_add(
    loom_cfg_condition_relation_anchor_builder_t* builder,
    loom_value_ordinal_t anchor, loom_cfg_condition_operand_t operand) {
  if (builder->pair_count >= builder->pair_capacity) {
    const iree_host_size_t minimum_capacity =
        builder->pair_capacity == 0 ? 16 : builder->pair_count + 1;
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        builder->arena, builder->pair_count, minimum_capacity,
        sizeof(*builder->pairs), &builder->pair_capacity,
        (void**)&builder->pairs));
  }
  builder->pairs[builder->pair_count++] =
      (loom_cfg_condition_relation_anchor_pair_t){
          .anchor = anchor,
          .operand = operand,
      };
  return iree_ok_status();
}

typedef struct loom_cfg_condition_relation_anchor_emit_state_t {
  // Builder receiving canonical sparse associations.
  loom_cfg_condition_relation_anchor_builder_t* builder;

  // Operand domain providing canonical identities and local ordinals.
  const loom_cfg_condition_operand_domain_t* operand_domain;

  // Authored dense relation operand receiving emitted anchors.
  loom_cfg_condition_operand_t operand;
} loom_cfg_condition_relation_anchor_emit_state_t;

static iree_status_t loom_cfg_condition_relation_anchor_emit(
    void* user_data, loom_value_id_t anchor_value_id) {
  loom_cfg_condition_relation_anchor_emit_state_t* state =
      (loom_cfg_condition_relation_anchor_emit_state_t*)user_data;
  const loom_cfg_condition_operand_domain_t* operand_domain =
      state->operand_domain;
  const loom_value_id_t canonical_anchor = loom_cfg_value_identity_table_lookup(
      operand_domain->identities, anchor_value_id);
  if (canonical_anchor == operand_domain->values[state->operand]) {
    return iree_ok_status();
  }
  const loom_value_ordinal_t anchor = loom_local_value_domain_ordinal(
      operand_domain->value_domain, canonical_anchor);
  return loom_cfg_condition_relation_anchor_builder_add(state->builder, anchor,
                                                        state->operand);
}

static iree_status_t loom_cfg_condition_relation_anchor_builder_finish(
    loom_cfg_condition_relation_anchor_builder_t* builder,
    loom_condition_relation_set_builder_t* set_builder) {
  if (builder->pair_count == 0) {
    return iree_ok_status();
  }
  loom_cfg_condition_relation_sort_anchor_pairs(builder->pairs,
                                                builder->pair_count);
  iree_host_size_t unique_pair_count = 0;
  for (iree_host_size_t i = 0; i < builder->pair_count; ++i) {
    if (unique_pair_count == 0 ||
        builder->pairs[i].anchor !=
            builder->pairs[unique_pair_count - 1].anchor ||
        builder->pairs[i].operand !=
            builder->pairs[unique_pair_count - 1].operand) {
      builder->pairs[unique_pair_count++] = builder->pairs[i];
    }
  }

  iree_host_size_t entry_count = 0;
  for (iree_host_size_t i = 0; i < unique_pair_count; ++i) {
    entry_count +=
        i == 0 || builder->pairs[i - 1].anchor != builder->pairs[i].anchor;
  }
  if (entry_count > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "condition relation anchors exceed uint32_t");
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(builder->arena, entry_count,
                                                 sizeof(*builder->entries),
                                                 (void**)&builder->entries));
  loom_cfg_condition_operand_t* operands = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      builder->arena, unique_pair_count, sizeof(*operands), (void**)&operands));

  iree_host_size_t pair_position = 0;
  uint32_t entry_position = 0;
  while (pair_position < unique_pair_count) {
    const loom_value_ordinal_t anchor = builder->pairs[pair_position].anchor;
    iree_host_size_t operand_count = 0;
    while (pair_position < unique_pair_count &&
           builder->pairs[pair_position].anchor == anchor) {
      operands[operand_count++] = builder->pairs[pair_position++].operand;
    }
    loom_condition_relation_set_id_t operand_set =
        LOOM_CONDITION_RELATION_SET_EMPTY;
    IREE_RETURN_IF_ERROR(loom_condition_relation_set_builder_intern(
        set_builder, operands, operand_count, &operand_set));
    builder->entries[entry_position++] =
        (loom_cfg_condition_relation_anchor_entry_t){
            .anchor = anchor,
            .operands = operand_set,
        };
  }
  IREE_ASSERT_EQ(entry_position, entry_count);
  builder->entry_count = entry_position;
  return iree_ok_status();
}

iree_status_t loom_cfg_condition_relation_anchor_builder_build(
    const loom_cfg_condition_relation_anchor_provider_t* provider,
    const loom_cfg_condition_operand_domain_t* operand_domain,
    const loom_cfg_condition_operand_t* relation_operands,
    uint32_t relation_operand_count,
    loom_condition_relation_set_builder_t* set_builder,
    iree_arena_allocator_t* scratch_arena,
    loom_cfg_condition_relation_anchor_builder_t** out_builder) {
  *out_builder = NULL;
  if (provider == NULL) {
    return iree_ok_status();
  }
  loom_cfg_condition_relation_anchor_builder_t* builder = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(scratch_arena, sizeof(*builder), (void**)&builder));
  memset(builder, 0, sizeof(*builder));
  builder->arena = scratch_arena;

  loom_cfg_condition_relation_anchor_emit_state_t state = {
      .builder = builder,
      .operand_domain = operand_domain,
  };
  const loom_cfg_condition_relation_anchor_sink_t sink = {
      .user_data = &state,
      .emit = loom_cfg_condition_relation_anchor_emit,
  };
  for (uint32_t i = 0; i < relation_operand_count; ++i) {
    const loom_cfg_condition_operand_t operand = relation_operands[i];
    state.operand = operand;
    IREE_RETURN_IF_ERROR(provider->query(
        provider->user_data, operand_domain->values[operand], &sink));
  }
  IREE_RETURN_IF_ERROR(
      loom_cfg_condition_relation_anchor_builder_finish(builder, set_builder));
  *out_builder = builder;
  return iree_ok_status();
}

uint32_t loom_cfg_condition_relation_anchor_builder_count(
    const loom_cfg_condition_relation_anchor_builder_t* builder) {
  return builder ? builder->entry_count : 0;
}

loom_condition_relation_set_id_t
loom_cfg_condition_relation_anchor_builder_root(
    const loom_cfg_condition_relation_anchor_builder_t* builder,
    uint32_t entry) {
  IREE_ASSERT_LT(entry, builder->entry_count);
  return builder->entries[entry].operands;
}

void loom_cfg_condition_relation_anchor_builder_set_root(
    loom_cfg_condition_relation_anchor_builder_t* builder, uint32_t entry,
    loom_condition_relation_set_id_t root) {
  IREE_ASSERT_LT(entry, builder->entry_count);
  builder->entries[entry].operands = root;
}

iree_status_t loom_cfg_condition_relation_anchor_builder_storage_size(
    const loom_cfg_condition_relation_anchor_builder_t* builder,
    iree_host_size_t* out_size) {
  if (!iree_host_size_checked_mul(
          loom_cfg_condition_relation_anchor_builder_count(builder),
          sizeof(loom_cfg_condition_relation_anchor_entry_t), out_size)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "condition relation anchors exceed host size");
  }
  return iree_ok_status();
}

void loom_cfg_condition_relation_anchor_builder_publish(
    const loom_cfg_condition_relation_anchor_builder_t* builder,
    void* storage) {
  const uint32_t entry_count =
      loom_cfg_condition_relation_anchor_builder_count(builder);
  if (entry_count == 0) {
    return;
  }
  memcpy(storage, builder->entries,
         (iree_host_size_t)entry_count * sizeof(*builder->entries));
}

loom_condition_relation_set_id_t
loom_cfg_condition_relation_anchor_index_lookup(
    const loom_cfg_condition_relation_anchor_index_t* index,
    uint32_t entry_count,
    const loom_cfg_condition_operand_domain_t* operand_domain,
    loom_value_id_t anchor_value_id) {
  if (entry_count == 0) {
    return LOOM_CONDITION_RELATION_SET_EMPTY;
  }
  const loom_value_id_t canonical = loom_cfg_value_identity_table_lookup(
      operand_domain->identities, anchor_value_id);
  const loom_value_ordinal_t anchor = loom_local_value_domain_try_ordinal(
      operand_domain->value_domain, canonical);
  if (anchor == LOOM_VALUE_ORDINAL_INVALID) {
    return LOOM_CONDITION_RELATION_SET_EMPTY;
  }
  const loom_cfg_condition_relation_anchor_entry_t* entries =
      (const loom_cfg_condition_relation_anchor_entry_t*)index;
  uint32_t begin = 0;
  uint32_t end = entry_count;
  while (begin < end) {
    const uint32_t middle = begin + (end - begin) / 2;
    if (entries[middle].anchor < anchor) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return begin < entry_count && entries[begin].anchor == anchor
             ? entries[begin].operands
             : LOOM_CONDITION_RELATION_SET_EMPTY;
}
