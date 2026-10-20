// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/loom-check/comparison.h"

typedef enum loom_check_match_kind_e {
  LOOM_CHECK_MATCH_PRESENT,
  LOOM_CHECK_MATCH_ABSENT,
  LOOM_CHECK_MATCH_COUNT,
} loom_check_match_kind_t;

static bool loom_check_parse_match_count(iree_string_view_t text,
                                         uint32_t* out_count) {
  uint32_t count = 0;
  for (iree_host_size_t i = 0; i < text.size; ++i) {
    const uint32_t digit = (uint32_t)(text.data[i] - '0');
    if (digit > 9 || count > (UINT32_MAX - digit) / 10) {
      return false;
    }
    count = count * 10 + digit;
  }
  *out_count = count;
  return count != 0;
}

static iree_status_t loom_check_match_lines(iree_string_view_t expected,
                                            loom_check_result_t* result) {
  bool has_positive_check = false;
  result->raw_outcome = LOOM_CHECK_PASS;
  iree_host_size_t line_number = 0;
  while (!iree_string_view_is_empty(expected)) {
    iree_string_view_t line;
    iree_string_view_split(expected, '\n', &line, &expected);
    ++line_number;
    line = iree_string_view_trim(line);
    if (iree_string_view_is_empty(line)) {
      continue;
    }
    if (iree_string_view_consume_prefix(&line, IREE_SV("//"))) {
      line = iree_string_view_trim(line);
      if (!iree_string_view_starts_with(line, IREE_SV("CHECK:")) &&
          !iree_string_view_starts_with(line, IREE_SV("CHECK-NOT:")) &&
          !iree_string_view_starts_with(line, IREE_SV("CHECK-COUNT-"))) {
        continue;
      }
    }
    loom_check_match_kind_t kind = LOOM_CHECK_MATCH_PRESENT;
    uint32_t expected_count = 0;
    if (iree_string_view_consume_prefix(&line, IREE_SV("CHECK:"))) {
      has_positive_check = true;
    } else if (iree_string_view_consume_prefix(&line, IREE_SV("CHECK-NOT:"))) {
      kind = LOOM_CHECK_MATCH_ABSENT;
    } else if (iree_string_view_consume_prefix(&line,
                                               IREE_SV("CHECK-COUNT-"))) {
      kind = LOOM_CHECK_MATCH_COUNT;
      iree_string_view_t count;
      if (iree_string_view_split(line, ':', &count, &line) < 0 ||
          !loom_check_parse_match_count(count, &expected_count)) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "expected positive count in CHECK-COUNT-N: at expected line "
            "%" PRIhsz,
            line_number);
      }
      has_positive_check = true;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "expected CHECK:, CHECK-NOT:, or CHECK-COUNT-N: "
                              "at expected line "
                              "%" PRIhsz,
                              line_number);
    }
    iree_string_view_t pattern = iree_string_view_trim(line);
    if (iree_string_view_is_empty(pattern)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "empty check pattern at expected line %" PRIhsz,
                              line_number);
    }

    iree_host_size_t matched_count = 0;
    iree_string_view_t output =
        iree_string_builder_view(&result->actual_output);
    while (!iree_string_view_is_empty(output)) {
      iree_string_view_t output_line;
      iree_string_view_split(output, '\n', &output_line, &output);
      output_line = iree_string_view_trim(output_line);
      if (!iree_string_view_match_pattern(output_line, pattern)) {
        continue;
      }
      ++matched_count;
      if (kind == LOOM_CHECK_MATCH_ABSENT) {
        IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
            &result->detail,
            "CHECK-NOT at expected line %" PRIhsz " matched: %.*s\n",
            line_number, (int)output_line.size, output_line.data));
      }
      if (kind != LOOM_CHECK_MATCH_COUNT) {
        break;
      }
    }
    if (kind == LOOM_CHECK_MATCH_COUNT && matched_count != expected_count) {
      result->raw_outcome = LOOM_CHECK_FAIL;
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          &result->detail,
          "CHECK-COUNT at expected line %" PRIhsz
          " expected %u matching lines, found %" PRIhsz ": %.*s\n",
          line_number, expected_count, matched_count, (int)pattern.size,
          pattern.data));
    } else if ((kind == LOOM_CHECK_MATCH_PRESENT && matched_count == 0) ||
               (kind == LOOM_CHECK_MATCH_ABSENT && matched_count != 0)) {
      result->raw_outcome = LOOM_CHECK_FAIL;
      if (kind == LOOM_CHECK_MATCH_PRESENT) {
        IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
            &result->detail,
            "CHECK at expected line %" PRIhsz " did not match: %.*s\n",
            line_number, (int)pattern.size, pattern.data));
      }
    }
  }
  if (!has_positive_check) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "with-checks requires at least one CHECK: or "
                            "CHECK-COUNT-N: pattern");
  }
  return iree_ok_status();
}

iree_status_t loom_check_compare_output(const loom_test_case_t* test_case,
                                        iree_allocator_t allocator,
                                        loom_check_result_t* result) {
  if (iree_all_bits_set(test_case->output_flags, LOOM_TEST_OUTPUT_CHECKS)) {
    return loom_check_match_lines(test_case->expected, result);
  }

  // Exact goldens ignore standalone comments and outer whitespace. The output
  // printer owns canonical blank-line placement within the comparable text.
  iree_string_builder_t stripped_expected;
  iree_string_builder_initialize(allocator, &stripped_expected);
  iree_status_t status =
      loom_test_file_remove_comments(test_case->expected, &stripped_expected);
  if (iree_status_is_ok(status)) {
    iree_string_view_t actual =
        iree_string_view_trim(iree_string_builder_view(&result->actual_output));
    iree_string_view_t expected =
        iree_string_view_trim(iree_string_builder_view(&stripped_expected));
    if (iree_string_view_equal(actual, expected)) {
      result->raw_outcome = LOOM_CHECK_PASS;
    } else {
      result->raw_outcome = LOOM_CHECK_FAIL;
      status =
          loom_check_result_record_diff(expected, actual, allocator, result);
    }
  }
  iree_string_builder_deinitialize(&stripped_expected);
  return status;
}
