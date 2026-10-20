// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/channel/ops.h"
#include "loom/ops/view/reference.h"

iree_status_t loom_channel_bind_facts(loom_fact_context_t* context,
                                      const loom_module_t* module,
                                      const loom_op_t* op,
                                      const loom_value_facts_t* operand_facts,
                                      loom_value_facts_t* result_facts) {
  const loom_value_id_t storage = loom_channel_bind_storage(op);
  return loom_view_reference_make_record(
      context, module, storage, operand_facts[0],
      loom_module_value_type(module, storage), &result_facts[0]);
}

iree_status_t loom_channel_access_facts(loom_fact_context_t* context,
                                        const loom_module_t* module,
                                        const loom_op_t* op,
                                        const loom_value_facts_t* operand_facts,
                                        loom_value_facts_t* result_facts) {
  (void)context;
  (void)module;
  // Each result refers to the binding's record storage. Equal spatial facts
  // never merge the channel identities or the independent owned obligations.
  for (uint16_t i = 0; i < op->result_count; ++i) {
    result_facts[i] = operand_facts[0];
  }
  return iree_ok_status();
}

iree_status_t loom_channel_select_facts(loom_fact_context_t* context,
                                        const loom_module_t* module,
                                        const loom_op_t* op,
                                        const loom_value_facts_t* operand_facts,
                                        loom_value_facts_t* result_facts) {
  (void)context;
  (void)module;
  (void)op;
  result_facts[0] = (loom_value_facts_t){0};
  result_facts[1] = operand_facts[0];
  return iree_ok_status();
}
