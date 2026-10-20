// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared scalar semantics for address-domain casts.

#ifndef LOOM_OPS_INDEX_CAST_H_
#define LOOM_OPS_INDEX_CAST_H_

#include "loom/ir/facts.h"
#include "loom/ir/scalar_type.h"

#ifdef __cplusplus
extern "C" {
#endif

// Applies index.cast semantics to one scalar fact summary. |input_type| and
// |result_type| must be fixed-width integer, index, or offset scalar types,
// with at least one side in an address domain.
loom_value_facts_t loom_index_cast_transfer_facts(
    loom_scalar_type_t input_type, loom_scalar_type_t result_type,
    loom_value_facts_t input_facts);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_INDEX_CAST_H_
