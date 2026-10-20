// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Sparse semantic anchors for indexed CFG condition relations.

#ifndef LOOM_ANALYSIS_CFG_CONDITION_RELATION_ANCHORS_H_
#define LOOM_ANALYSIS_CFG_CONDITION_RELATION_ANCHORS_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/analysis/cfg_condition_operand_domain.h"
#include "loom/analysis/condition_relation_set.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits one SSA value that should retrieve relations involving a derived
// relation operand.
typedef iree_status_t (*loom_cfg_condition_relation_anchor_emit_fn_t)(
    void* user_data, loom_value_id_t anchor_value_id);

// Sink receiving the semantic anchors for one relation operand.
typedef struct loom_cfg_condition_relation_anchor_sink_t {
  // Opaque construction state passed to |emit|.
  void* user_data;

  // Emits one semantic anchor value.
  loom_cfg_condition_relation_anchor_emit_fn_t emit;
} loom_cfg_condition_relation_anchor_sink_t;

// Enumerates semantic anchors for one SSA relation operand. Implementations
// may expand producer arithmetic while the relation table is constructed; the
// table retains the resulting incidence so queries never revisit producers.
typedef iree_status_t (*loom_cfg_condition_relation_anchor_query_fn_t)(
    void* user_data, loom_value_id_t relation_value_id,
    const loom_cfg_condition_relation_anchor_sink_t* sink);

// Optional producer-specific relation-anchor query.
typedef struct loom_cfg_condition_relation_anchor_provider_t {
  // Provider-owned query state.
  void* user_data;

  // Enumerates semantic anchors for one relation operand.
  loom_cfg_condition_relation_anchor_query_fn_t query;
} loom_cfg_condition_relation_anchor_provider_t;

typedef struct loom_cfg_condition_relation_anchor_builder_t
    loom_cfg_condition_relation_anchor_builder_t;
typedef struct loom_cfg_condition_relation_anchor_index_t
    loom_cfg_condition_relation_anchor_index_t;

// Builds canonical sparse anchor associations for |relation_operands| in
// |operand_domain|. Authored operands remain the retained relation values;
// emitted values are query keys only.
iree_status_t loom_cfg_condition_relation_anchor_builder_build(
    const loom_cfg_condition_relation_anchor_provider_t* provider,
    const loom_cfg_condition_operand_domain_t* operand_domain,
    const loom_cfg_condition_operand_t* relation_operands,
    uint32_t relation_operand_count,
    loom_condition_relation_set_builder_t* set_builder,
    iree_arena_allocator_t* scratch_arena,
    loom_cfg_condition_relation_anchor_builder_t** out_builder);

// Returns the number of sparse anchor entries in |builder|.
uint32_t loom_cfg_condition_relation_anchor_builder_count(
    const loom_cfg_condition_relation_anchor_builder_t* builder);

// Returns one construction set root for immutable set publication.
loom_condition_relation_set_id_t
loom_cfg_condition_relation_anchor_builder_root(
    const loom_cfg_condition_relation_anchor_builder_t* builder,
    uint32_t entry);

// Replaces one construction set root with its published set root.
void loom_cfg_condition_relation_anchor_builder_set_root(
    loom_cfg_condition_relation_anchor_builder_t* builder, uint32_t entry,
    loom_condition_relation_set_id_t root);

// Computes the exact retained byte size required by |builder|.
iree_status_t loom_cfg_condition_relation_anchor_builder_storage_size(
    const loom_cfg_condition_relation_anchor_builder_t* builder,
    iree_host_size_t* out_size);

// Copies |builder| into exact caller-provided retained storage.
void loom_cfg_condition_relation_anchor_builder_publish(
    const loom_cfg_condition_relation_anchor_builder_t* builder, void* storage);

// Returns the authored relation operands associated with |anchor_value_id|.
loom_condition_relation_set_id_t
loom_cfg_condition_relation_anchor_index_lookup(
    const loom_cfg_condition_relation_anchor_index_t* index,
    uint32_t entry_count,
    const loom_cfg_condition_operand_domain_t* operand_domain,
    loom_value_id_t anchor_value_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_CFG_CONDITION_RELATION_ANCHORS_H_
