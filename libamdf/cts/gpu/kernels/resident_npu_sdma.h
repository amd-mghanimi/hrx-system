// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_RESIDENT_NPU_SDMA_H_
#define AMDF_CTS_GPU_KERNELS_RESIDENT_NPU_SDMA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::resident_npu_sdma {

// Addresses remain borrowed through participant completion and SDMA command
// retirement.
struct Arguments {
  // GPU return slot R, including its generation.
  uint64_t request;
  // NPU request slot Q, including its generation.
  uint64_t response;
  // Separately maintained host startup decision.
  uint64_t startup;
  // Guarded final-acknowledgement allocation.
  uint64_t control;
  // Complete request, copy and command-frontier transcript.
  uint64_t records;
  // Borrowed device address of the SDMA command ring.
  uint64_t ring;
  // Borrowed 64-bit SDMA command-consumption index.
  uint64_t read_index;
  // Borrowed 64-bit canonical command-publication index.
  uint64_t write_index;
  // Borrowed write-only 64-bit SDMA notification address.
  uint64_t notification;
  // GPU-readable copied payload after its leading guard.
  uint64_t destination;
  // GPU-readable SDMA FENCE generation after its leading guard.
  uint64_t completion;
  // GPU base of eight immutable source pages, used in packets.
  uint64_t source_address;
  // Packet operand naming the same payload as destination.
  uint64_t destination_address;
  // Packet operand naming the same generation as completion.
  uint64_t completion_address;
  // Power-of-two SDMA ring capacity in bytes.
  uint64_t capacity;
  // Distance in bytes between guarded source pages.
  uint64_t source_stride;
  // GPU return count; a positive count adds one closing Q row.
  uint32_t round_count;
  // Complete payload length in 32-bit words, from 1 to 1024.
  uint32_t payload_word_count;
  // Q/R payload word offset from its generation: 1 or 16.
  uint32_t payload_word_offset;
  // Family-selected COPY_LINEAR control word.
  uint32_t copy_control;
  // Family-selected FENCE header word.
  uint32_t fence_header;
  // Queried SDMA USER_GCR acquire bit 0 and release bit 1.
  uint32_t cache_flags;
};

inline constexpr uint32_t kArgumentByteLength =
    offsetof(Arguments, cache_flags) + sizeof(uint32_t);
inline constexpr std::array<uint32_t, 22> kArgumentByteOffsets = {
    offsetof(Arguments, request),
    offsetof(Arguments, response),
    offsetof(Arguments, startup),
    offsetof(Arguments, control),
    offsetof(Arguments, records),
    offsetof(Arguments, ring),
    offsetof(Arguments, read_index),
    offsetof(Arguments, write_index),
    offsetof(Arguments, notification),
    offsetof(Arguments, destination),
    offsetof(Arguments, completion),
    offsetof(Arguments, source_address),
    offsetof(Arguments, destination_address),
    offsetof(Arguments, completion_address),
    offsetof(Arguments, capacity),
    offsetof(Arguments, source_stride),
    offsetof(Arguments, round_count),
    offsetof(Arguments, payload_word_count),
    offsetof(Arguments, payload_word_offset),
    offsetof(Arguments, copy_control),
    offsetof(Arguments, fence_header),
    offsetof(Arguments, cache_flags),
};
inline constexpr std::array<uint32_t, 22> kArgumentByteLengths = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4};
inline constexpr std::array<std::string_view, 22> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "global_buffer", "global_buffer",
    "global_buffer", "global_buffer", "global_buffer", "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value",      "by_value",      "by_value",
    "by_value",      "by_value"};

static_assert(kArgumentByteLength == 152);
static_assert(sizeof(Arguments) == 152);

}  // namespace kernels::resident_npu_sdma

#endif  // AMDF_CTS_GPU_KERNELS_RESIDENT_NPU_SDMA_H_
