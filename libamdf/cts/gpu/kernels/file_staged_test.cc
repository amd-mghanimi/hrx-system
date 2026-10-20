// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/kernels/file_staged.h"

#include <array>
#include <string_view>

#include "libamdf/cts/gpu/kernels/file_staged_read_kernels.h"
#include "libamdf/cts/gpu/kernels/file_staged_reader_kernels.h"
#include "libamdf/cts/gpu/kernels/file_staged_upload_kernels.h"
#include "libamdf/cts/gpu/kernels/product_test.h"

namespace {

TEST(FileStagedKernelTest, ReadProductsPreserveTheCallerContract) {
  using Arguments = kernels::file_staged::ReadArguments;
  constexpr std::array<uint32_t, 16> kOffsets = {
      offsetof(Arguments, submission_entries),
      offsetof(Arguments, submission_tail),
      offsetof(Arguments, completion_entries),
      offsetof(Arguments, completion_head),
      offsetof(Arguments, completion_tail),
      offsetof(Arguments, state),
      offsetof(Arguments, result),
      offsetof(Arguments, request),
      offsetof(Arguments, previous_reader),
      offsetof(Arguments, host_payload),
      offsetof(Arguments, submission_mask),
      offsetof(Arguments, completion_mask),
      offsetof(Arguments, word_count),
      offsetof(Arguments, file_index),
      offsetof(Arguments, generation),
      offsetof(Arguments, file_block_mask)};
  constexpr std::array<uint32_t, 16> kLengths = {8, 8, 8, 8, 8, 8, 8, 8,
                                                 8, 8, 4, 4, 4, 4, 4, 4};
  constexpr std::array<std::string_view, 16> kKinds = {
      "global_buffer", "global_buffer", "global_buffer", "global_buffer",
      "global_buffer", "global_buffer", "global_buffer", "global_buffer",
      "global_buffer", "by_value",      "by_value",      "by_value",
      "by_value",      "by_value",      "by_value",      "by_value"};
  for (const auto& kernel : kernels::file_staged_read::kKernels.variants) {
    SCOPED_TRACE(kernel.target);
    kernels::testing::CheckArgumentLayout(kernel, kOffsets, kLengths, kKinds,
                                          sizeof(Arguments),
                                          alignof(Arguments));
    EXPECT_EQ(kernel.arguments.byte_length, 104u);
    EXPECT_EQ(kernel.required_workgroup_size,
              (std::array<uint32_t, 3>{1, 1, 1}));
    EXPECT_EQ(kernel.private_segment_byte_length, 0u);
    EXPECT_EQ(kernel.group_segment_byte_length, 0u);
  }
}

TEST(FileStagedKernelTest, UploadProductsPreserveTheCallerContract) {
  using Arguments = kernels::file_staged::UploadArguments;
  constexpr std::array<uint32_t, 15> kOffsets = {
      offsetof(Arguments, ring),
      offsetof(Arguments, read_index),
      offsetof(Arguments, write_index),
      offsetof(Arguments, notification),
      offsetof(Arguments, completion),
      offsetof(Arguments, state),
      offsetof(Arguments, result),
      offsetof(Arguments, source),
      offsetof(Arguments, destination),
      offsetof(Arguments, completion_address),
      offsetof(Arguments, capacity),
      offsetof(Arguments, generation),
      offsetof(Arguments, copy_control),
      offsetof(Arguments, fence_header),
      offsetof(Arguments, cache_flags)};
  constexpr std::array<uint32_t, 15> kLengths = {8, 8, 8, 8, 8, 8, 8, 8,
                                                 8, 8, 8, 4, 4, 4, 4};
  constexpr std::array<std::string_view, 15> kKinds = {
      "global_buffer", "global_buffer", "global_buffer", "global_buffer",
      "global_buffer", "global_buffer", "global_buffer", "by_value",
      "by_value",      "by_value",      "by_value",      "by_value",
      "by_value",      "by_value",      "by_value"};
  for (const auto& kernel : kernels::file_staged_upload::kKernels.variants) {
    SCOPED_TRACE(kernel.target);
    kernels::testing::CheckArgumentLayout(kernel, kOffsets, kLengths, kKinds,
                                          sizeof(Arguments),
                                          alignof(Arguments));
    EXPECT_EQ(kernel.arguments.byte_length, 104u);
    EXPECT_EQ(kernel.required_workgroup_size,
              (std::array<uint32_t, 3>{1, 1, 1}));
    EXPECT_EQ(kernel.private_segment_byte_length, 0u);
    EXPECT_EQ(kernel.group_segment_byte_length, 0u);
  }
}

TEST(FileStagedKernelTest, ReaderProductsPreserveTheCallerContract) {
  using Arguments = kernels::file_staged::ReaderArguments;
  constexpr std::array<uint32_t, 4> kOffsets = {
      offsetof(Arguments, input), offsetof(Arguments, output),
      offsetof(Arguments, result), offsetof(Arguments, reader)};
  constexpr std::array<uint32_t, 4> kLengths = {8, 8, 8, 4};
  constexpr std::array<std::string_view, 4> kKinds = {
      "global_buffer", "global_buffer", "global_buffer", "by_value"};
  for (const auto& kernel : kernels::file_staged_reader::kKernels.variants) {
    SCOPED_TRACE(kernel.target);
    kernels::testing::CheckArgumentLayout(kernel, kOffsets, kLengths, kKinds,
                                          sizeof(Arguments),
                                          alignof(Arguments));
    EXPECT_EQ(kernel.arguments.byte_length, 32u);
    EXPECT_EQ(kernel.required_workgroup_size,
              (std::array<uint32_t, 3>{64, 1, 1}));
    EXPECT_EQ(kernel.private_segment_byte_length, 0u);
    EXPECT_EQ(kernel.group_segment_byte_length, 0u);
  }
}

}  // namespace
