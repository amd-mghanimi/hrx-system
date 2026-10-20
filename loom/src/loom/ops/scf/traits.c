// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/scf/ops.h"

loom_trait_flags_t loom_scf_select_effective_traits(const loom_module_t* module,
                                                    const loom_op_t* op) {
  loom_trait_flags_t traits = op->traits & ~LOOM_TRAIT_DECOMPOSABLE;
  const loom_type_t result_type =
      loom_module_value_type(module, loom_scf_select_result(op));
  if (loom_type_is_vector(result_type)) {
    traits |= LOOM_TRAIT_DECOMPOSABLE;
  }
  return traits;
}
