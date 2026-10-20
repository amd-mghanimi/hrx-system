// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/symbol_value_constraints.h"

#include <inttypes.h>
#include <math.h>

#include "loom/ir/attribute.h"
#include "loom/ir/float_facts.h"

static bool loom_symbol_value_to_i64(loom_type_t type, loom_attribute_t value,
                                     int64_t* out_value) {
  if (!loom_type_is_scalar(type)) {
    return false;
  }
  loom_scalar_type_t scalar_type = loom_type_element_type(type);
  if (!loom_attr_matches_scalar_type(value, scalar_type, NULL)) {
    return false;
  }
  if (scalar_type == LOOM_SCALAR_TYPE_I1 && value.kind == LOOM_ATTR_BOOL) {
    *out_value = loom_attr_as_bool(value) ? 1 : 0;
    return true;
  }
  if (scalar_type == LOOM_SCALAR_TYPE_I1 ||
      scalar_type == LOOM_SCALAR_TYPE_INDEX ||
      scalar_type == LOOM_SCALAR_TYPE_OFFSET ||
      loom_scalar_type_is_integer(scalar_type)) {
    *out_value = loom_attr_as_i64(value);
    return true;
  }
  return false;
}

static iree_status_t loom_symbol_value_predicate_const_arg(
    const loom_predicate_t* predicate, uint8_t arg_index, int64_t* out_value) {
  if (arg_index >= predicate->arg_count ||
      predicate->arg_tags[arg_index] != LOOM_PRED_ARG_CONST) {
    const char* kind_name = loom_predicate_kind_name(predicate->kind);
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "symbol value constraint '%s' must use literal integer arguments",
        kind_name ? kind_name : "<unknown>");
  }
  *out_value = predicate->args[arg_index];
  return iree_ok_status();
}

static bool loom_symbol_value_is_power_of_two(int64_t value) {
  return value > 0 && (value & (value - 1)) == 0;
}

static iree_status_t loom_symbol_value_integer_predicate_satisfied(
    const loom_predicate_t* predicate, int64_t value, bool* out_satisfied) {
  uint8_t expected_arg_count =
      loom_predicate_kind_argument_count(predicate->kind);
  if (expected_arg_count == UINT8_MAX ||
      predicate->arg_count != expected_arg_count) {
    const char* kind_name = loom_predicate_kind_name(predicate->kind);
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "symbol value constraint '%s' has invalid argument count %u",
        kind_name ? kind_name : "<unknown>", (unsigned)predicate->arg_count);
  }

  int64_t constant = 0;
  switch ((loom_predicate_kind_t)predicate->kind) {
    case LOOM_PREDICATE_EQ: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value == constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_NE: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value != constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_LT: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value < constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_LE: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value <= constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_GT: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value > constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_GE: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value >= constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_ULT: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = (uint64_t)value < (uint64_t)constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_ULE: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = (uint64_t)value <= (uint64_t)constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_UGT: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = (uint64_t)value > (uint64_t)constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_UGE: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = (uint64_t)value >= (uint64_t)constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_MULTIPLE_OF: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      if (constant <= 0) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "symbol value constraint 'multiple_of' requires a positive "
            "divisor");
      }
      *out_satisfied = value % constant == 0;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_POWER_OF_TWO:
      *out_satisfied = loom_symbol_value_is_power_of_two(value);
      return iree_ok_status();
    case LOOM_PREDICATE_RANGE: {
      int64_t lower = 0;
      int64_t upper = 0;
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &lower));
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 2, &upper));
      *out_satisfied = value >= lower && value <= upper;
      return iree_ok_status();
    }
    default:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown symbol value constraint kind %u",
                              (unsigned)predicate->kind);
  }
}

static iree_status_t loom_symbol_value_float_predicate_satisfied(
    const loom_predicate_t* predicate, double value, bool* out_satisfied) {
  const uint8_t expected_arg_count =
      loom_predicate_kind_argument_count(predicate->kind);
  if (expected_arg_count == UINT8_MAX ||
      predicate->arg_count != expected_arg_count) {
    const char* kind_name = loom_predicate_kind_name(predicate->kind);
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "symbol value constraint '%s' has invalid argument count %u",
        kind_name ? kind_name : "<unknown>", (unsigned)predicate->arg_count);
  }

  int64_t constant = 0;
  switch ((loom_predicate_kind_t)predicate->kind) {
    case LOOM_PREDICATE_EQ: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value == (double)constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_NE: {
      IREE_RETURN_IF_ERROR(
          loom_symbol_value_predicate_const_arg(predicate, 1, &constant));
      *out_satisfied = value != (double)constant;
      return iree_ok_status();
    }
    case LOOM_PREDICATE_NOT_NAN:
      *out_satisfied = !isnan(value);
      return iree_ok_status();
    case LOOM_PREDICATE_NOT_INF:
      *out_satisfied = !isinf(value);
      return iree_ok_status();
    case LOOM_PREDICATE_FINITE:
      *out_satisfied = isfinite(value);
      return iree_ok_status();
    default: {
      const char* kind_name = loom_predicate_kind_name(predicate->kind);
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "symbol value constraint '%s' does not accept a floating-point "
          "value",
          kind_name ? kind_name : "<unknown>");
    }
  }
}

iree_status_t loom_symbol_value_constraints_check_exact(
    iree_string_view_t symbol_name, loom_type_t type,
    loom_value_id_t contract_value, loom_attribute_t exact_value,
    loom_attribute_t predicates) {
  if (predicates.kind == LOOM_ATTR_ABSENT || predicates.count == 0) {
    return iree_ok_status();
  }
  if (predicates.kind != LOOM_ATTR_PREDICATE_LIST ||
      !predicates.predicate_list) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "symbol '@%.*s' has invalid value constraints",
                            (int)symbol_name.size, symbol_name.data);
  }

  if (!loom_type_is_scalar(type)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "symbol '@%.*s' has value constraints on a non-scalar type",
        (int)symbol_name.size, symbol_name.data);
  }
  const loom_scalar_type_t scalar_type = loom_type_element_type(type);
  if (!loom_attr_matches_scalar_type(exact_value, scalar_type, NULL)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "symbol '@%.*s' exact value does not match its "
                            "scalar type",
                            (int)symbol_name.size, symbol_name.data);
  }

  const bool is_float = loom_scalar_type_is_float(scalar_type);
  int64_t integer_value = 0;
  double float_value = 0.0;
  if (is_float) {
    const loom_value_facts_t exact_facts = loom_value_facts_exact_float(
        scalar_type, loom_attr_as_f64(exact_value));
    const bool has_exact_value =
        loom_value_facts_as_exact_float(scalar_type, exact_facts, &float_value);
    IREE_ASSERT(has_exact_value);
  } else if (!loom_symbol_value_to_i64(type, exact_value, &integer_value)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "symbol '@%.*s' has constraints on an unsupported scalar type",
        (int)symbol_name.size, symbol_name.data);
  }

  for (uint16_t i = 0; i < predicates.count; ++i) {
    const loom_predicate_t* predicate = &predicates.predicate_list[i];
    if (predicate->arg_count == 0 ||
        predicate->arg_tags[0] != LOOM_PRED_ARG_VALUE ||
        predicate->args[0] < 0 ||
        (loom_value_id_t)predicate->args[0] != contract_value) {
      continue;
    }
    bool satisfied = false;
    if (is_float) {
      IREE_RETURN_IF_ERROR(loom_symbol_value_float_predicate_satisfied(
          predicate, float_value, &satisfied));
    } else {
      IREE_RETURN_IF_ERROR(loom_symbol_value_integer_predicate_satisfied(
          predicate, integer_value, &satisfied));
    }
    if (satisfied) {
      continue;
    }
    const char* kind_name = loom_predicate_kind_name(predicate->kind);
    if (is_float) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "symbol '@%.*s' value %.17g violates constraint "
                              "'%s'",
                              (int)symbol_name.size, symbol_name.data,
                              float_value, kind_name ? kind_name : "<unknown>");
    }
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "symbol '@%.*s' value %" PRId64
                            " violates constraint '%s'",
                            (int)symbol_name.size, symbol_name.data,
                            integer_value, kind_name ? kind_name : "<unknown>");
  }
  return iree_ok_status();
}
