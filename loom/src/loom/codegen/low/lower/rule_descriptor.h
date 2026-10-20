// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-low descriptor bindings and result representation queries used by
// generated rule selection, emission, and value materializers.

#ifndef LOOM_CODEGEN_LOW_LOWER_RULE_DESCRIPTOR_H_
#define LOOM_CODEGEN_LOW_LOWER_RULE_DESCRIPTOR_H_

#include "iree/base/api.h"
#include "loom/codegen/low/lower/lower.h"
#include "loom/codegen/low/lower/rules.h"

#ifdef __cplusplus
extern "C" {
#endif

// Descriptor bindings shared by immutable rule-table and descriptor-set
// identities. Function-specific facts never participate in this cache.
typedef struct loom_low_lower_rule_descriptor_cache_t {
  // Owner of this cache and its lazily allocated ordinal arrays.
  iree_arena_allocator_t* arena;
  // Next distinct table binding in the owning lowering scope.
  struct loom_low_lower_rule_descriptor_cache_t* next;
  // Immutable rule-table list whose ordinals index the trailing map pointers.
  loom_low_lower_rule_set_list_t rule_sets;
  // Immutable descriptor set whose row ordinals populate the maps.
  const loom_low_descriptor_set_t* descriptor_set;
} loom_low_lower_rule_descriptor_cache_t;

// Selects the cache for this immutable table pair, moving it to the head of
// |inout_cache| or allocating it there on first use. Cache addresses remain
// stable. The list contains only configured table identities, independent of
// the number of source functions or values using them.
iree_status_t loom_low_lower_rule_descriptor_cache_select(
    loom_low_lower_rule_set_list_t rule_sets,
    const loom_low_descriptor_set_t* descriptor_set,
    iree_arena_allocator_t* arena,
    loom_low_lower_rule_descriptor_cache_t** inout_cache);

// Resolves one rule-local descriptor ref, retaining present and absent results.
// The rule's ordinal array is allocated at its exact extent on first use.
iree_status_t loom_low_lower_rule_descriptor_cache_resolve(
    loom_low_lower_rule_descriptor_cache_t* cache, uint16_t rule_set_index,
    loom_low_lower_descriptor_ref_t descriptor_ref,
    const loom_low_descriptor_t** out_descriptor);

// Returns the trusted descriptor operand row for one result ordinal.
const loom_low_operand_t* loom_low_lower_rule_descriptor_result_operand(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_descriptor_t* descriptor, uint16_t result_index);

// Returns the trusted descriptor row for an SSA packet operand ordinal.
// Descriptor-only state and immediate rows do not consume these ordinals.
const loom_low_operand_t* loom_low_lower_rule_descriptor_packet_operand(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_descriptor_t* descriptor, uint16_t operand_index);

// Materializes the register type declared by one trusted descriptor result.
iree_status_t loom_low_lower_rule_descriptor_result_type(
    loom_low_lower_context_t* context, const loom_low_descriptor_t* descriptor,
    uint16_t result_index, loom_type_t* out_type);

// Returns the register type for a copied packet operand. The descriptor's
// concrete register class is used when the source class is not accepted while
// preserving any semantic value type carried by the source register.
iree_status_t loom_low_lower_rule_descriptor_copy_operand_type(
    loom_low_lower_context_t* context, const loom_low_descriptor_t* descriptor,
    uint16_t operand_index, loom_type_t source_type, loom_type_t* out_type);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_RULE_DESCRIPTOR_H_
