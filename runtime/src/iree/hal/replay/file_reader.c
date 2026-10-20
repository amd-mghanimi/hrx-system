// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/replay/file_reader.h"

#include <stddef.h>
#include <string.h>

#include "iree/hal/replay/digest.h"

#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "IREE HAL replay file serialization requires little-endian hosts"
#endif  // !IREE_ENDIANNESS_LITTLE

IREE_API_EXPORT iree_status_t
iree_hal_replay_file_parse_header(iree_const_byte_span_t file_contents,
                                  iree_hal_replay_file_header_t* out_header,
                                  iree_host_size_t* out_record_offset) {
  IREE_ASSERT_ARGUMENT(out_header);
  IREE_ASSERT_ARGUMENT(out_record_offset);
  memset(out_header, 0, sizeof(*out_header));
  *out_record_offset = 0;

  if (IREE_UNLIKELY(file_contents.data_length > 0 && !file_contents.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "replay file contents data is required");
  }
  if (IREE_UNLIKELY(file_contents.data_length <
                    sizeof(iree_hal_replay_file_header_t))) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file is too small for header");
  }

  iree_hal_replay_file_header_t header;
  memcpy(&header, file_contents.data, sizeof(header));
  if (IREE_UNLIKELY(header.magic != IREE_HAL_REPLAY_FILE_MAGIC)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid IREE HAL replay file magic");
  }
  if (IREE_UNLIKELY(header.version_major !=
                    IREE_HAL_REPLAY_FILE_VERSION_MAJOR)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unsupported IREE HAL replay file version");
  }
  if (IREE_UNLIKELY(header.version_minor >
                    IREE_HAL_REPLAY_FILE_VERSION_MINOR)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unsupported IREE HAL replay file version");
  }
  if (IREE_UNLIKELY(header.header_length < sizeof(header))) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file header length is too small");
  }
  if (IREE_UNLIKELY(header.header_length > file_contents.data_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file header extends past file end");
  }
  if (IREE_UNLIKELY(header.flags != 0)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file reserved flags must be zero");
  }
  if (IREE_UNLIKELY(header.file_length != 0 &&
                    header.file_length < header.header_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file length is before the first record");
  }
  if (IREE_UNLIKELY(header.file_length > file_contents.data_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file length extends past file contents");
  }

  *out_header = header;
  *out_record_offset = header.header_length;
  return iree_ok_status();
}

// Validates each nested extent once at the file boundary. Executors and dumpers
// can then consume the borrowed records without repeating structural checks.
static iree_status_t iree_hal_replay_file_parse_barrier_list(
    uint64_t count, bool has_transition_recipes,
    iree_const_byte_span_t* remaining,
    iree_hal_replay_barrier_list_view_t* out_list) {
  out_list->count = count;
  out_list->payload = iree_make_const_byte_span(remaining->data, 0);
  if (count == UINT64_MAX) {
    return iree_ok_status();
  }
  for (uint64_t i = 0; i < count; ++i) {
    iree_hal_replay_command_buffer_execution_barrier_payload_t header;
    if (remaining->data_length < sizeof(header)) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "replay barrier header is truncated");
    }
    memcpy(&header, remaining->data, sizeof(header));
    iree_host_size_t memory_size = 0, buffer_size = 0, recipe_size = 0;
    iree_host_size_t operation_count = 0, operation_size = 0, total_size = 0;
    if (header.source_stage_mask > UINT32_MAX ||
        header.target_stage_mask > UINT32_MAX ||
        header.memory_barrier_count > IREE_HOST_SIZE_MAX ||
        header.buffer_barrier_count > IREE_HOST_SIZE_MAX ||
        !iree_host_size_checked_mul(
            (iree_host_size_t)header.memory_barrier_count,
            sizeof(iree_hal_replay_memory_barrier_payload_t), &memory_size) ||
        !iree_host_size_checked_mul(
            (iree_host_size_t)header.buffer_barrier_count,
            sizeof(iree_hal_replay_buffer_barrier_payload_t), &buffer_size) ||
        !iree_host_size_checked_add(sizeof(header), memory_size, &total_size) ||
        !iree_host_size_checked_add(total_size, buffer_size, &total_size)) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "replay barrier extent is invalid");
    }
    if (has_transition_recipes) {
      if (!iree_host_size_checked_mul(
              (iree_host_size_t)header.buffer_barrier_count,
              sizeof(iree_hal_replay_memory_transition_recipe_payload_t),
              &recipe_size) ||
          !iree_host_size_checked_add(total_size, recipe_size, &total_size) ||
          total_size > remaining->data_length) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "replay barrier recipe extent is invalid");
      }
      const uint8_t* recipe_data = remaining->data + total_size - recipe_size;
      for (iree_host_size_t j = 0;
           j < (iree_host_size_t)header.buffer_barrier_count; ++j) {
        iree_hal_replay_memory_transition_recipe_payload_t recipe;
        memcpy(&recipe, recipe_data + j * sizeof(recipe), sizeof(recipe));
        if (IREE_UNLIKELY((recipe.effects == 0) !=
                          (recipe.operation_count == 0))) {
          return iree_make_status(
              IREE_STATUS_DATA_LOSS,
              "replay barrier recipe effects and operations disagree");
        }
        if (!iree_host_size_checked_add(operation_count, recipe.operation_count,
                                        &operation_count)) {
          return iree_make_status(
              IREE_STATUS_DATA_LOSS,
              "replay barrier recipe operation count overflows");
        }
      }
      if (!iree_host_size_checked_mul(
              operation_count,
              sizeof(iree_hal_replay_memory_transition_operation_payload_t),
              &operation_size) ||
          !iree_host_size_checked_add(total_size, operation_size,
                                      &total_size)) {
        return iree_make_status(
            IREE_STATUS_DATA_LOSS,
            "replay barrier recipe operation extent is invalid");
      }
    }
    if (total_size > remaining->data_length) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "replay barrier extent is invalid");
    }
    remaining->data += total_size;
    remaining->data_length -= total_size;
    out_list->payload.data_length += total_size;
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_replay_file_parse_queue_barriers(
    iree_hal_replay_file_record_t* record) {
  if (record->header.record_type !=
          IREE_HAL_REPLAY_FILE_RECORD_TYPE_OPERATION ||
      !iree_hal_replay_operation_has_queue_barriers(
          record->header.operation_code)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay operation cannot carry queue barriers");
  }
  iree_hal_replay_queue_barriers_footer_t footer;
  if (record->payload.data_length < sizeof(footer)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay queue barrier footer is truncated");
  }
  iree_host_size_t footer_offset = record->payload.data_length - sizeof(footer);
  memcpy(&footer, record->payload.data + footer_offset, sizeof(footer));
  if (footer.payload_length > footer_offset) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay queue barrier extension is truncated");
  }
  iree_host_size_t barrier_offset = footer_offset - footer.payload_length;
  iree_const_byte_span_t remaining = iree_make_const_byte_span(
      record->payload.data + barrier_offset, footer.payload_length);
  const bool has_transition_recipes = iree_any_bit_set(
      record->header.record_flags,
      IREE_HAL_REPLAY_FILE_RECORD_FLAG_MEMORY_TRANSITION_RECIPES);
  IREE_RETURN_IF_ERROR(iree_hal_replay_file_parse_barrier_list(
      footer.before_count, has_transition_recipes, &remaining,
      &record->barriers.before));
  IREE_RETURN_IF_ERROR(iree_hal_replay_file_parse_barrier_list(
      footer.after_count, has_transition_recipes, &remaining,
      &record->barriers.after));
  if (remaining.data_length) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "replay queue barrier extension has trailing bytes");
  }
  record->payload.data_length = barrier_offset;
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_hal_replay_file_parse_record(
    iree_const_byte_span_t file_contents, iree_host_size_t record_offset,
    iree_hal_replay_file_record_t* out_record,
    iree_host_size_t* out_next_record_offset) {
  IREE_ASSERT_ARGUMENT(out_record);
  IREE_ASSERT_ARGUMENT(out_next_record_offset);
  memset(out_record, 0, sizeof(*out_record));
  *out_next_record_offset = record_offset;

  if (IREE_UNLIKELY(file_contents.data_length > 0 && !file_contents.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "replay file contents data is required");
  }
  if (IREE_UNLIKELY(record_offset >= file_contents.data_length)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "replay record offset is outside the file");
  }

  iree_host_size_t remaining_file_length =
      file_contents.data_length - record_offset;
  if (IREE_UNLIKELY(remaining_file_length <
                    sizeof(iree_hal_replay_file_record_header_t))) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record is too small for header");
  }

  const uint8_t* record_base = file_contents.data + record_offset;
  iree_hal_replay_file_record_header_t header;
  memcpy(&header, record_base, sizeof(header));
  const iree_hal_replay_file_record_flags_t valid_flags =
      IREE_HAL_REPLAY_FILE_RECORD_FLAG_OPTIONAL |
      IREE_HAL_REPLAY_FILE_RECORD_FLAG_QUEUE_BARRIERS |
      IREE_HAL_REPLAY_FILE_RECORD_FLAG_MEMORY_TRANSITION_RECIPES;
  if (IREE_UNLIKELY((header.record_flags & ~valid_flags) != 0)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record reserved flags must be zero");
  }
  const bool is_command_buffer_barrier =
      header.record_type == IREE_HAL_REPLAY_FILE_RECORD_TYPE_OPERATION &&
      header.operation_code ==
          IREE_HAL_REPLAY_OPERATION_CODE_COMMAND_BUFFER_EXECUTION_BARRIER;
  if (IREE_UNLIKELY(
          iree_any_bit_set(
              header.record_flags,
              IREE_HAL_REPLAY_FILE_RECORD_FLAG_MEMORY_TRANSITION_RECIPES) &&
          !iree_any_bit_set(header.record_flags,
                            IREE_HAL_REPLAY_FILE_RECORD_FLAG_QUEUE_BARRIERS) &&
          !is_command_buffer_barrier)) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "replay transition recipes require a barrier-bearing operation");
  }
  if (IREE_UNLIKELY(
          !iree_hal_replay_file_record_type_is_known(header.record_type) &&
          !iree_all_bits_set(header.record_flags,
                             IREE_HAL_REPLAY_FILE_RECORD_FLAG_OPTIONAL))) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "unknown required replay record type");
  }
  if (IREE_UNLIKELY(header.reserved0 != 0 || header.reserved1 != 0)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record reserved fields must be zero");
  }
  if (IREE_UNLIKELY(header.record_length > IREE_HOST_SIZE_MAX)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record length exceeds host size");
  }
  iree_host_size_t record_length = (iree_host_size_t)header.record_length;
  if (IREE_UNLIKELY(record_length > remaining_file_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record extends past file end");
  }
  if (IREE_UNLIKELY(header.header_length < sizeof(header) ||
                    header.header_length > record_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record header length is invalid");
  }
  if (IREE_UNLIKELY(header.payload_length > IREE_HOST_SIZE_MAX)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record payload length exceeds host size");
  }
  if (IREE_UNLIKELY((iree_host_size_t)header.payload_length !=
                    record_length - header.header_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay record payload length does not match");
  }

  const uint8_t* payload = record_base + header.header_length;
  out_record->header = header;
  out_record->payload = iree_make_const_byte_span(
      payload, (iree_host_size_t)header.payload_length);
  if (iree_any_bit_set(header.record_flags,
                       IREE_HAL_REPLAY_FILE_RECORD_FLAG_QUEUE_BARRIERS)) {
    IREE_RETURN_IF_ERROR(iree_hal_replay_file_parse_queue_barriers(out_record));
  }
  if (header.record_type == IREE_HAL_REPLAY_FILE_RECORD_TYPE_OPERATION &&
      header.payload_type ==
          IREE_HAL_REPLAY_PAYLOAD_TYPE_COMMAND_BUFFER_EXECUTION_BARRIER &&
      header.status_code == IREE_STATUS_OK) {
    iree_const_byte_span_t remaining = out_record->payload;
    iree_hal_replay_barrier_list_view_t barrier;
    const bool has_transition_recipes = iree_any_bit_set(
        header.record_flags,
        IREE_HAL_REPLAY_FILE_RECORD_FLAG_MEMORY_TRANSITION_RECIPES);
    IREE_RETURN_IF_ERROR(iree_hal_replay_file_parse_barrier_list(
        1, has_transition_recipes, &remaining, &barrier));
    if (remaining.data_length) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "replay barrier payload has trailing bytes");
    }
  }
  *out_next_record_offset = record_offset + record_length;
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t
iree_hal_replay_file_range_validate(iree_const_byte_span_t file_contents,
                                    const iree_hal_replay_file_range_t* range) {
  IREE_ASSERT_ARGUMENT(range);

  if (IREE_UNLIKELY(file_contents.data_length > 0 && !file_contents.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "replay file contents data is required");
  }
  if (IREE_UNLIKELY(range->flags != 0 || range->reserved0 != 0)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file range reserved fields must be zero");
  }
  if (IREE_UNLIKELY(range->compression_type !=
                    IREE_HAL_REPLAY_COMPRESSION_TYPE_NONE)) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "unsupported replay file range compression");
  }
  if (IREE_UNLIKELY(range->uncompressed_length != range->length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "uncompressed replay file range length mismatch");
  }
  if (IREE_UNLIKELY(range->offset > UINT64_MAX - range->length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file range end offset overflow");
  }
  uint64_t end_offset = range->offset + range->length;
  if (IREE_UNLIKELY(end_offset > file_contents.data_length)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file range extends past file contents");
  }
  if (IREE_UNLIKELY(range->length > IREE_HOST_SIZE_MAX)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "replay file range length exceeds host size");
  }

  iree_host_size_t range_offset = (iree_host_size_t)range->offset;
  iree_const_byte_span_t contents = iree_make_const_byte_span(
      file_contents.data + range_offset, (iree_host_size_t)range->length);
  switch (range->digest_type) {
    case IREE_HAL_REPLAY_DIGEST_TYPE_NONE:
      if (IREE_UNLIKELY(!iree_hal_replay_digest_is_zero(range->digest))) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "replay file range digest bytes must be zero");
      }
      return iree_ok_status();
    case IREE_HAL_REPLAY_DIGEST_TYPE_FNV1A_64: {
      const uint64_t expected_digest =
          iree_hal_replay_digest_load_fnv1a64(range->digest);
      uint64_t actual_digest = iree_hal_replay_digest_fnv1a64_update(
          iree_hal_replay_digest_fnv1a64_initialize(), contents);
      if (IREE_UNLIKELY(actual_digest != expected_digest)) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "replay file range digest mismatch");
      }
      return iree_ok_status();
    }
    default:
      return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                              "unsupported replay file range digest");
  }
}
