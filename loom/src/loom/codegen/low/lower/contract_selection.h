// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Allocation-free iteration over generated composed-contract candidates.

#ifndef LOOM_CODEGEN_LOW_LOWER_CONTRACT_SELECTION_H_
#define LOOM_CODEGEN_LOW_LOWER_CONTRACT_SELECTION_H_

#include "loom/target/contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t loom_low_lower_contract_case_iteration_mode_t;

enum loom_low_lower_contract_case_iteration_mode_e {
  // Iterates the generated exact-key candidate sequence when present.
  LOOM_LOW_LOWER_CONTRACT_CASE_ITERATION_CANDIDATES = 0,
  // Iterates every composed case in authored priority order.
  LOOM_LOW_LOWER_CONTRACT_CASE_ITERATION_ALL = 1,
};

typedef struct loom_low_lower_contract_case_iterator_t {
  // Selected candidate words, or NULL for contiguous case iteration.
  const uint32_t* candidate_words;
  // Unconsumed bits from the current priority-bitmap word.
  uint32_t remaining_bitmap;
  // Absolute composed index of relative case ordinal zero.
  uint16_t case_start;
  // Number of list candidates, bitmap words, or contiguous cases.
  uint16_t candidate_count;
  // Next list candidate, bitmap word, or contiguous case position.
  uint16_t candidate_position;
  // Relative case ordinal of the current priority-bitmap word.
  uint16_t bitmap_ordinal_base;
  // Internal candidate sequence encoding.
  uint8_t encoding;
} loom_low_lower_contract_case_iterator_t;

// Initializes an iterator for |entry| and returns true when candidate mode
// selected a generated exact-key sequence. Type selectors observe
// |vector_lane_projection| so the candidate set and subsequent rule matching
// use the same scoped types. All mode preserves the full composed order used
// for exact failure diagnostics and returns false.
bool loom_low_lower_contract_case_iterator_initialize(
    const loom_module_t* module, const loom_target_contract_index_t* index,
    loom_target_contract_op_entry_t entry, const loom_op_t* source_op,
    loom_target_contract_vector_lane_projection_t vector_lane_projection,
    loom_low_lower_contract_case_iteration_mode_t mode,
    loom_low_lower_contract_case_iterator_t* out_iterator);

// Advances |iterator| and returns the next absolute composed case index.
bool loom_low_lower_contract_case_iterator_next(
    loom_low_lower_contract_case_iterator_t* iterator,
    uint16_t* out_case_index);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_CONTRACT_SELECTION_H_
