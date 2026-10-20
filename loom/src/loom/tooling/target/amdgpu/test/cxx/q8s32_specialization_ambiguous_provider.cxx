// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/target.h>

#include "q8s32_specialization.h"

// Equal applicability and priority intentionally make the imported provider
// library ambiguous when this diagnostic overlay is present.
LOOM_TEMPLATE_DEF(q8s32_project)
[[loom::priority(40)]] float q8s32_wave32_values32_ambiguous(
    unsigned input_capacity, Q8S32Codes16 weights_low,
    Q8S32Codes16 weights_high, Q8S32BFloat16 activations_low,
    Q8S32BFloat16 activations_high, std::bfloat16_t scale)
    [[loom::where(loom::target::subgroup::size() == 32u &&
                  loom::predicate::multiple_of(input_capacity, 512u))]] {
  return 0.0f;
}
