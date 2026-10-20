// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/vector/reduction_legalization.h"

#include <stdint.h>

#include "loom/ir/module.h"
#include "loom/ops/vector/combining.h"
#include "loom/ops/vector/ops.h"

typedef struct loom_vector_reduce_axes_vector_state_t {
  loom_rewriter_t* rewriter;
  loom_op_t* op;
  loom_value_id_t fold_source;
  loom_type_t flat_result_type;
  loom_combining_kind_t kind;
  uint64_t term_lane_count;
  uint8_t fastmath_flags;
} loom_vector_reduce_axes_vector_state_t;

static bool loom_vector_reduce_axes_contains_axis(loom_attribute_t axes,
                                                  uint8_t axis) {
  for (uint16_t i = 0; i < axes.count; ++i) {
    if (axes.i64_array[i] == axis) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_vector_reduce_axes_static_vector_type(
    loom_module_t* module, loom_type_t prototype, const uint64_t* dimensions,
    uint8_t rank, loom_type_t* out_type) {
  loom_type_t type = prototype;
  uint8_t flags = LOOM_TYPE_FLAG_ALL_STATIC;
  if (rank <= 2) {
    flags |= LOOM_TYPE_FLAG_INLINE_DIMS;
  }
  type.header = loom_type_make_header(
      LOOM_TYPE_VECTOR, loom_type_element_type(prototype), rank, flags);
  type.dims[0] = 0;
  type.dims[1] = 0;
  if (rank <= 2) {
    for (uint8_t i = 0; i < rank; ++i) {
      type.dims[i] = dimensions[i];
    }
  } else {
    type.dims[0] = (uint64_t)(uintptr_t)dimensions;
  }
  return loom_module_intern_type(module, type, out_type);
}

static iree_status_t loom_vector_reduce_axes_build_term(
    loom_vector_reduce_axes_vector_state_t* state, uint64_t ordinal,
    loom_value_id_t* out_term) {
  const int64_t static_offset = (int64_t)(ordinal * state->term_lane_count);
  loom_op_t* slice_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_slice_build(
      &state->rewriter->builder, state->fold_source, NULL, 0, &static_offset, 1,
      state->flat_result_type, state->op->location, &slice_op));
  *out_term = loom_vector_slice_result(slice_op);
  return iree_ok_status();
}

static iree_status_t loom_vector_reduce_axes_build_tree(
    loom_vector_reduce_axes_vector_state_t* state, uint64_t begin_ordinal,
    uint64_t end_ordinal, loom_value_id_t* out_result) {
  if (end_ordinal - begin_ordinal == 1) {
    return loom_vector_reduce_axes_build_term(state, begin_ordinal, out_result);
  }
  const uint64_t middle_ordinal =
      begin_ordinal + (end_ordinal - begin_ordinal) / 2;
  loom_value_id_t lhs = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_build_tree(
      state, begin_ordinal, middle_ordinal, &lhs));
  loom_value_id_t rhs = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_build_tree(state, middle_ordinal,
                                                          end_ordinal, &rhs));
  return loom_vector_combining_build(
      &state->rewriter->builder, state->kind, state->fastmath_flags, lhs, rhs,
      state->flat_result_type, state->op->location, out_result);
}

static iree_status_t loom_vector_reduce_axes_build_fold(
    loom_vector_reduce_axes_vector_state_t* state, uint64_t term_count,
    loom_value_id_t init, loom_value_id_t* out_result) {
  const bool can_reassociate =
      loom_combining_kind_accepts_integer(state->kind) ||
      iree_any_bit_set(state->fastmath_flags,
                       LOOM_VECTOR_FASTMATHFLAGS_REASSOC);
  if (can_reassociate) {
    loom_value_id_t tree = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_vector_reduce_axes_build_tree(state, 0, term_count, &tree));
    return loom_vector_combining_build(
        &state->rewriter->builder, state->kind, state->fastmath_flags, init,
        tree, state->flat_result_type, state->op->location, out_result);
  }

  loom_value_id_t accumulator = init;
  for (uint64_t ordinal = 0; ordinal < term_count; ++ordinal) {
    loom_value_id_t term = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_vector_reduce_axes_build_term(state, ordinal, &term));
    IREE_RETURN_IF_ERROR(loom_vector_combining_build(
        &state->rewriter->builder, state->kind, state->fastmath_flags,
        accumulator, term, state->flat_result_type, state->op->location,
        &accumulator));
  }
  *out_result = accumulator;
  return iree_ok_status();
}

static iree_status_t loom_vector_reduce_axes_replace(
    loom_rewriter_t* rewriter, loom_op_t* op, loom_value_id_t replacement,
    loom_value_id_t value_checkpoint) {
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement,
                                                  1);
}

iree_status_t loom_vector_reduce_axes_to_vector_rewrite_op(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_rewritten) {
  *out_rewritten = false;
  if (!loom_vector_reduce_axes_isa(op)) {
    return iree_ok_status();
  }

  const loom_value_id_t input = loom_vector_reduce_axes_input(op);
  const loom_value_id_t init = loom_vector_reduce_axes_init(op);
  const loom_type_t input_type =
      loom_module_value_type(rewriter->module, input);
  if (!loom_type_is_all_static(input_type)) {
    return iree_ok_status();
  }

  const loom_attribute_t axes = loom_vector_reduce_axes_axes(op);
  const uint8_t input_rank = loom_type_rank(input_type);
  if (axes.count == input_rank) {
    return iree_ok_status();
  }
  const loom_type_t result_type = loom_module_value_type(
      rewriter->module, loom_vector_reduce_axes_result(op));
  if (!loom_type_is_all_static(result_type)) {
    return iree_ok_status();
  }

  uint64_t input_element_count = 0;
  uint64_t result_element_count = 0;
  if (!loom_type_static_element_count(input_type, &input_element_count) ||
      !loom_type_static_element_count(result_type, &result_element_count)) {
    return iree_ok_status();
  }
  if (input_element_count == 0 || result_element_count == 0) {
    IREE_RETURN_IF_ERROR(
        loom_rewriter_replace_all_uses_and_erase(rewriter, op, &init, 1));
    *out_rewritten = true;
    return iree_ok_status();
  }
  const uint64_t term_count = input_element_count / result_element_count;
  if (input_element_count > INT64_MAX || result_element_count > INT64_MAX) {
    return iree_ok_status();
  }

  int64_t permutation[LOOM_TYPE_MAX_RANK] = {0};
  uint8_t permutation_position = 0;
  for (uint16_t i = 0; i < axes.count; ++i) {
    permutation[permutation_position++] = axes.i64_array[i];
  }
  for (uint8_t axis = 0; axis < input_rank; ++axis) {
    if (!loom_vector_reduce_axes_contains_axis(axes, axis)) {
      permutation[permutation_position++] = axis;
    }
  }
  bool permutation_is_identity = true;
  for (uint8_t i = 0; i < input_rank; ++i) {
    permutation_is_identity &= permutation[i] == i;
  }

  loom_builder_set_before(&rewriter->builder, op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  loom_value_id_t permuted_input = input;
  loom_type_t permuted_type = input_type;
  if (!permutation_is_identity) {
    uint64_t permuted_dimensions[LOOM_TYPE_MAX_RANK] = {0};
    for (uint8_t i = 0; i < input_rank; ++i) {
      permuted_dimensions[i] =
          loom_type_dim(input_type, (uint8_t)permutation[i]);
    }
    IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_static_vector_type(
        rewriter->module, input_type, permuted_dimensions, input_rank,
        &permuted_type));
    loom_op_t* transpose_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_transpose_build(
        &rewriter->builder, permutation, input_rank, input, permuted_type,
        op->location, &transpose_op));
    permuted_input = loom_vector_transpose_result(transpose_op);
  }

  const uint64_t flat_result_dimension =
      loom_dim_pack_static((int64_t)result_element_count);
  loom_type_t flat_result_type = {0};
  IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_static_vector_type(
      rewriter->module, result_type, &flat_result_dimension, 1,
      &flat_result_type));

  const uint64_t flat_input_dimension =
      loom_dim_pack_static((int64_t)input_element_count);
  loom_type_t flat_input_type = {0};
  IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_static_vector_type(
      rewriter->module, permuted_type, &flat_input_dimension, 1,
      &flat_input_type));

  loom_value_id_t fold_source = permuted_input;
  if (!loom_type_equal(permuted_type, flat_input_type)) {
    loom_op_t* bitcast_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_bitcast_build(
        &rewriter->builder, permuted_input, permuted_type, flat_input_type,
        op->location, &bitcast_op));
    fold_source = loom_vector_bitcast_result(bitcast_op);
  }

  loom_value_id_t flat_init = init;
  if (!loom_type_equal(result_type, flat_result_type)) {
    loom_op_t* bitcast_op = NULL;
    IREE_RETURN_IF_ERROR(
        loom_vector_bitcast_build(&rewriter->builder, init, result_type,
                                  flat_result_type, op->location, &bitcast_op));
    flat_init = loom_vector_bitcast_result(bitcast_op);
  }

  loom_vector_reduce_axes_vector_state_t state = {
      .rewriter = rewriter,
      .op = op,
      .fold_source = fold_source,
      .flat_result_type = flat_result_type,
      .kind = loom_vector_reduce_axes_kind(op),
      .term_lane_count = result_element_count,
      .fastmath_flags = loom_vector_reduce_axes_fastmath(op),
  };
  loom_value_id_t flat_result = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_build_fold(
      &state, term_count, flat_init, &flat_result));

  loom_value_id_t replacement = flat_result;
  if (!loom_type_equal(flat_result_type, result_type)) {
    loom_op_t* bitcast_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_bitcast_build(
        &rewriter->builder, flat_result, flat_result_type, result_type,
        op->location, &bitcast_op));
    replacement = loom_vector_bitcast_result(bitcast_op);
  }
  IREE_RETURN_IF_ERROR(loom_vector_reduce_axes_replace(
      rewriter, op, replacement, value_checkpoint));
  *out_rewritten = true;
  return iree_ok_status();
}
