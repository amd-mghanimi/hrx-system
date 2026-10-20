// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/task_abi.h"

#include "loom/ir/module.h"
#include "loom/ops/kernel/ops.h"
#include "loom/ops/low/ops.h"
#include "loom/target/abi/task/state_layout.h"

typedef struct loom_low_task_kernel_imports_t {
  // Native index carrier shared by invocation-constant imports. None until the
  // first builtin is selected; emission never requests a new type mapping.
  loom_type_t index_type;
  // First source query for each builtin, retained during operation selection.
  // NULL entries require no import. Preamble emission binds these values once;
  // all other queries alias the corresponding canonical source value.
  const loom_op_t* sources[LOOM_TASK_BUILTIN_COUNT_];
} loom_low_task_kernel_imports_t;

static const char loom_low_task_kernel_imports_key;

static loom_low_lower_plan_id_t loom_low_task_kernel_builtin_id(
    const loom_op_t* op) {
  if (loom_kernel_workgroup_id_isa(op)) {
    return LOOM_TASK_BUILTIN_WORKGROUP_ID_X +
           loom_kernel_workgroup_id_dimension(op);
  }
  if (loom_kernel_workgroup_count_isa(op)) {
    return LOOM_TASK_BUILTIN_WORKGROUP_COUNT_X +
           loom_kernel_workgroup_count_dimension(op);
  }
  return LOOM_LOW_LOWER_PLAN_ID_NONE;
}

iree_status_t loom_low_task_select_kernel_builtin(
    void* user_data, loom_low_lower_context_t* context,
    const loom_op_t* source_op, loom_low_lower_plan_t* out_plan) {
  (void)user_data;
  *out_plan = loom_low_lower_plan_empty();
  const loom_low_lower_plan_id_t id =
      loom_low_task_kernel_builtin_id(source_op);
  if (id == LOOM_LOW_LOWER_PLAN_ID_NONE ||
      loom_low_lower_context_bundle(context)->export_plan->abi_kind !=
          LOOM_TARGET_ABI_HAL_KERNEL) {
    return iree_ok_status();
  }
  loom_low_task_kernel_imports_t* imports = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_get_or_allocate_target_state(
      context, &loom_low_task_kernel_imports_key, sizeof(*imports),
      (void**)&imports));
  if (loom_type_kind(imports->index_type) == LOOM_TYPE_NONE) {
    IREE_RETURN_IF_ERROR(loom_low_lower_map_type(
        context, source_op, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
        &imports->index_type));
    if (loom_type_kind(imports->index_type) == LOOM_TYPE_NONE) {
      return iree_ok_status();
    }
  }
  if (!imports->sources[id]) {
    imports->sources[id] = source_op;
  }
  IREE_RETURN_IF_ERROR(loom_low_lower_plan_value_type(
      context, loom_op_const_results(source_op)[0], imports->index_type));
  *out_plan = loom_low_lower_plan_make(id, imports);
  return iree_ok_status();
}

iree_status_t loom_low_task_emit_kernel_preamble(
    void* user_data, loom_low_lower_context_t* context) {
  (void)user_data;
  const loom_low_task_kernel_imports_t* imports =
      loom_low_lower_lookup_target_state(
          context, &loom_low_task_kernel_imports_key, sizeof(*imports));
  if (!imports) {
    return iree_ok_status();
  }
  loom_module_t* module = loom_low_lower_context_module(context);
  iree_status_t status = iree_ok_status();
  for (unsigned i = 0;
       i < LOOM_TASK_BUILTIN_COUNT_ && iree_status_is_ok(status); ++i) {
    const loom_op_t* source = imports->sources[i];
    if (!source) {
      continue;
    }
    loom_string_id_t name;
    status =
        loom_module_intern_string(module, loom_task_builtins[i].name, &name);
    loom_op_t* live_in = NULL;
    if (iree_status_is_ok(status)) {
      status =
          loom_low_live_in_build(loom_low_lower_context_builder(context), 0,
                                 name, loom_named_attr_slice_empty(),
                                 loom_low_lower_value_binding_type(
                                     context, loom_op_const_results(source)[0]),
                                 source->location, &live_in);
    }
    if (iree_status_is_ok(status)) {
      status =
          loom_low_lower_bind_value(context, loom_op_const_results(source)[0],
                                    loom_low_live_in_result(live_in));
    }
  }
  return status;
}

iree_status_t loom_low_task_emit_kernel_builtin(
    void* user_data, loom_low_lower_context_t* context,
    const loom_op_t* source_op, loom_low_lower_plan_t plan) {
  (void)user_data;
  const loom_low_task_kernel_imports_t* imports = plan.target_data;
  return loom_low_lower_bind_value_alias(
      context, loom_op_const_results(imports->sources[plan.id])[0],
      loom_op_const_results(source_op)[0]);
}

iree_status_t loom_low_task_query_kernel_builtin(
    void* user_data,
    const loom_target_contract_query_environment_t* environment,
    const loom_op_t* source_op,
    loom_target_contract_query_result_t* out_result) {
  (void)user_data;
  *out_result = loom_target_contract_query_result_empty();
  if (loom_target_contract_query_environment_bundle(environment)
              ->export_plan->abi_kind == LOOM_TARGET_ABI_HAL_KERNEL &&
      loom_low_task_kernel_builtin_id(source_op) !=
          LOOM_LOW_LOWER_PLAN_ID_NONE) {
    out_result->outcome = LOOM_TARGET_CONTRACT_QUERY_LEGAL;
  }
  return iree_ok_status();
}
