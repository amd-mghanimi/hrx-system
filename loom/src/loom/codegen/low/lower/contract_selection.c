// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/contract_selection.h"

#include "iree/base/internal/math.h"
#include "loom/ir/context.h"

// Generated selection data is a relocation-free uint32_t blob:
//
//   header: row_count:u16 | bucket_count:u16
//   minimum indexed case count:u16 | reserved:u16
//   row[row_count]:
//     selector:u16 | bucket_start:u16
//     bucket_count:u16 | fallback_word_start:u16
//     fallback_encoded_count:u16 | source_op_kind:u16
//   bucket[bucket_count]: key:u32, candidate_span:u32
//   candidate_words[]
//
// A candidate span packs word_start:u16 | encoded_count:u16. The count's high
// bit selects a priority bitmap; otherwise the words contain two u16 case
// ordinals each. Op entries carry the row ordinal and absolute case base.

enum loom_low_lower_contract_candidate_encoding_e {
  LOOM_LOW_LOWER_CONTRACT_CANDIDATE_CONTIGUOUS = 0,
  LOOM_LOW_LOWER_CONTRACT_CANDIDATE_LIST = 1,
  LOOM_LOW_LOWER_CONTRACT_CANDIDATE_BITMAP = 2,
};

enum loom_low_lower_contract_selector_kind_e {
  LOOM_LOW_LOWER_CONTRACT_SELECTOR_ENUM_ATTRIBUTE = 1,
  LOOM_LOW_LOWER_CONTRACT_SELECTOR_OPERAND_TYPE = 2,
  LOOM_LOW_LOWER_CONTRACT_SELECTOR_RESULT_TYPE = 3,
};

enum {
  LOOM_LOW_LOWER_CONTRACT_CANDIDATE_BITMAP_BIT = 0x8000u,
  LOOM_LOW_LOWER_CONTRACT_TYPE_KEY_SCALAR = 1u << 28,
  LOOM_LOW_LOWER_CONTRACT_TYPE_KEY_VECTOR = 2u << 28,
};

static bool loom_low_lower_contract_type_key(loom_type_t type,
                                             uint32_t* out_key) {
  if (loom_type_is_scalar(type)) {
    *out_key = LOOM_LOW_LOWER_CONTRACT_TYPE_KEY_SCALAR |
               ((uint32_t)loom_type_element_type(type) << 16);
    return true;
  }
  if (!loom_type_is_vector(type) || loom_type_rank(type) != 1 ||
      !loom_type_is_all_static(type)) {
    return false;
  }
  const int64_t lane_count = loom_type_dim_static_size_at(type, 0);
  if (lane_count < 0 || lane_count > UINT16_MAX) {
    return false;
  }
  *out_key = LOOM_LOW_LOWER_CONTRACT_TYPE_KEY_VECTOR |
             ((uint32_t)loom_type_element_type(type) << 16) |
             (uint32_t)lane_count;
  return true;
}

static bool loom_low_lower_contract_selector_key(
    const loom_module_t* module, const loom_op_t* source_op,
    loom_target_contract_vector_lane_projection_t vector_lane_projection,
    uint16_t selector, uint32_t* out_key) {
  const uint8_t kind = selector & 0x3u;
  const uint8_t field_index = (uint8_t)((selector >> 2) & 0xFFu);
  const uint8_t element_index = (uint8_t)((selector >> 10) & 0x3Fu);
  if (kind == LOOM_LOW_LOWER_CONTRACT_SELECTOR_ENUM_ATTRIBUTE) {
    if (field_index >= source_op->attribute_count) {
      return false;
    }
    const loom_attribute_t attribute =
        loom_op_const_attrs(source_op)[field_index];
    if (attribute.kind != LOOM_ATTR_ENUM) {
      return false;
    }
    *out_key = attribute.raw;
    return true;
  }

  const loom_op_vtable_t* vtable = loom_op_vtable(module, source_op);
  loom_value_slice_t span;
  if (kind == LOOM_LOW_LOWER_CONTRACT_SELECTOR_OPERAND_TYPE) {
    span = loom_op_operand_field_span(vtable, source_op, field_index);
  } else if (kind == LOOM_LOW_LOWER_CONTRACT_SELECTOR_RESULT_TYPE) {
    span = loom_op_result_field_span(vtable, source_op, field_index);
  } else {
    IREE_ASSERT_UNREACHABLE("unknown contract candidate selector kind");
    IREE_BUILTIN_UNREACHABLE();
  }
  if (element_index >= span.count) {
    return false;
  }
  return loom_low_lower_contract_type_key(
      loom_target_contract_query_value_type(vector_lane_projection, module,
                                            span.values[element_index]),
      out_key);
}

static void loom_low_lower_contract_case_iterator_select_span(
    const uint32_t* candidate_words, uint32_t packed_span,
    loom_low_lower_contract_case_iterator_t* iterator) {
  const uint16_t word_start = packed_span & 0xFFFFu;
  const uint16_t encoded_count = packed_span >> 16;
  iterator->candidate_words = candidate_words + word_start;
  iterator->candidate_count =
      encoded_count & ~LOOM_LOW_LOWER_CONTRACT_CANDIDATE_BITMAP_BIT;
  iterator->encoding =
      (encoded_count & LOOM_LOW_LOWER_CONTRACT_CANDIDATE_BITMAP_BIT) != 0
          ? LOOM_LOW_LOWER_CONTRACT_CANDIDATE_BITMAP
          : LOOM_LOW_LOWER_CONTRACT_CANDIDATE_LIST;
}

bool loom_low_lower_contract_case_iterator_initialize(
    const loom_module_t* module, const loom_target_contract_index_t* index,
    loom_target_contract_op_entry_t entry, const loom_op_t* source_op,
    loom_target_contract_vector_lane_projection_t vector_lane_projection,
    loom_low_lower_contract_case_iteration_mode_t mode,
    loom_low_lower_contract_case_iterator_t* out_iterator) {
  *out_iterator = (loom_low_lower_contract_case_iterator_t){
      .candidate_words = NULL,
      .remaining_bitmap = 0,
      .case_start = entry.case_start,
      .candidate_count = entry.case_count,
      .candidate_position = 0,
      .bitmap_ordinal_base = 0,
      .encoding = LOOM_LOW_LOWER_CONTRACT_CANDIDATE_CONTIGUOUS,
  };
  if (mode == LOOM_LOW_LOWER_CONTRACT_CASE_ITERATION_ALL ||
      index->selection_data == NULL) {
    return false;
  }

  const uint32_t* selection_data = index->selection_data;
  const uint16_t row_count = selection_data[0] & 0xFFFFu;
  const uint16_t bucket_count = selection_data[0] >> 16;
  const uint16_t minimum_case_count = selection_data[1] & 0xFFFFu;
  if (entry.case_count < minimum_case_count) {
    return false;
  }
  const uint32_t* rows = selection_data + 2;
  uint16_t lower_bound = 0;
  uint16_t upper_bound = row_count;
  while (lower_bound < upper_bound) {
    const uint16_t middle = lower_bound + (upper_bound - lower_bound) / 2;
    const loom_op_kind_t row_op_kind = rows[middle * 3 + 2] >> 16;
    if (row_op_kind < source_op->kind) {
      lower_bound = middle + 1;
    } else {
      upper_bound = middle;
    }
  }
  if (lower_bound >= row_count ||
      (rows[lower_bound * 3 + 2] >> 16) != source_op->kind) {
    return false;
  }
  const uint32_t* row = rows + lower_bound * 3;
  const uint32_t* buckets = rows + row_count * 3;
  const uint32_t* candidate_words = buckets + bucket_count * 2;

  uint32_t packed_span = (row[1] >> 16) | ((row[2] & 0xFFFFu) << 16);
  uint32_t key = 0;
  if (loom_low_lower_contract_selector_key(
          module, source_op, vector_lane_projection, row[0] & 0xFFFFu, &key)) {
    uint16_t bucket_lower_bound = row[0] >> 16;
    uint16_t bucket_upper_bound = bucket_lower_bound + (row[1] & 0xFFFFu);
    while (bucket_lower_bound < bucket_upper_bound) {
      const uint16_t middle =
          bucket_lower_bound + (bucket_upper_bound - bucket_lower_bound) / 2;
      const uint32_t bucket_key = buckets[middle * 2];
      if (bucket_key < key) {
        bucket_lower_bound = middle + 1;
      } else {
        bucket_upper_bound = middle;
      }
    }
    const uint16_t bucket_end = (row[0] >> 16) + (row[1] & 0xFFFFu);
    if (bucket_lower_bound < bucket_end &&
        buckets[bucket_lower_bound * 2] == key) {
      packed_span = buckets[bucket_lower_bound * 2 + 1];
    }
  }
  loom_low_lower_contract_case_iterator_select_span(candidate_words,
                                                    packed_span, out_iterator);
  return true;
}

bool loom_low_lower_contract_case_iterator_next(
    loom_low_lower_contract_case_iterator_t* iterator,
    uint16_t* out_case_index) {
  if (iterator->encoding == LOOM_LOW_LOWER_CONTRACT_CANDIDATE_CONTIGUOUS) {
    if (iterator->candidate_position >= iterator->candidate_count) {
      return false;
    }
    *out_case_index = iterator->case_start + iterator->candidate_position++;
    return true;
  }
  if (iterator->encoding == LOOM_LOW_LOWER_CONTRACT_CANDIDATE_LIST) {
    if (iterator->candidate_position >= iterator->candidate_count) {
      return false;
    }
    const uint16_t position = iterator->candidate_position++;
    const uint32_t word = iterator->candidate_words[position / 2];
    const uint16_t case_ordinal =
        position & 1u ? (uint16_t)(word >> 16) : (uint16_t)word;
    *out_case_index = iterator->case_start + case_ordinal;
    return true;
  }

  while (iterator->remaining_bitmap == 0) {
    if (iterator->candidate_position >= iterator->candidate_count) {
      return false;
    }
    iterator->bitmap_ordinal_base = iterator->candidate_position * 32u;
    iterator->remaining_bitmap =
        iterator->candidate_words[iterator->candidate_position++];
  }
  const uint16_t bit_ordinal =
      (uint16_t)iree_math_count_trailing_zeros_u32(iterator->remaining_bitmap);
  iterator->remaining_bitmap &= iterator->remaining_bitmap - 1u;
  *out_case_index =
      iterator->case_start + iterator->bitmap_ordinal_base + bit_ordinal;
  return true;
}
