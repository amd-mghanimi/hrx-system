// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_LOOKAHEAD_H_
#define AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_LOOKAHEAD_H_

#include <cstddef>
#include <cstdint>

namespace kernels::device_sdma_lookahead {

// Immutable publisher arguments. Native queue dependencies retire every reader
// of the assigned slot before this dispatch can overwrite it.
struct alignas(16) UploadArguments {
  // GPU view of the byte-indexed SDMA ring.
  uint64_t ring;
  // Aligned 64-bit consumed command frontier.
  uint64_t read_index;
  // Aligned 64-bit published command frontier.
  uint64_t write_index;
  // Write-only aligned 64-bit queue notification address.
  uint64_t notification;
  // Separate SDMA completion word, advanced once per upload.
  uint64_t completion;
  // Persistent 64-bit frontier, owned by the ordered publisher queue.
  uint64_t state;
  // This job's descriptor, published before its native completion signal.
  uint64_t selection;
  // One immutable token-like input from which the GPU selects page and length.
  uint64_t request;
  // Prior job's 320-byte signal/marker block, used only for observation.
  uint64_t previous_readers;
  // Four payload word counts in [1, 18432], within each guarded slot.
  uint64_t lengths;
  // Eight immutable guarded source pages with payloads at byte 64.
  uint64_t source_address;
  // Assigned guarded input slot, with payload at byte 64.
  uint64_t input_address;
  // Numeric SDMA address of the same word as completion.
  uint64_t completion_address;
  // Power-of-two ring byte capacity in [4096, 2^32].
  uint64_t capacity;
  // Byte stride of the source pages and input slots.
  uint64_t slot_byte_length;
  // Assigned cyclic input slot, retained until all readers complete.
  uint32_t slot;
  // Positive upload ordinal, unique during the finite batch.
  uint32_t generation;
  // Family-selected scope fields in COPY_LINEAR DWORD 2.
  uint32_t copy_control;
  // Family-selected complete FENCE header.
  uint32_t fence_header;
  // Queried USER_GCR acquire (bit 0) and release (bit 1).
  uint32_t cache_flags;
  // Zero-filled tail of the kernel ABI's eight-byte-aligned argument extent.
  uint32_t reserved;
};
static_assert(sizeof(UploadArguments) == 144);
static_assert(offsetof(UploadArguments, slot) == 120);
static_assert(offsetof(UploadArguments, cache_flags) == 136);

// Independent readers borrow the same input and retain separate outputs.
struct alignas(16) ReaderArguments {
  // Assigned input payload, after its guarded prefix.
  uint64_t input;
  // Per-job, per-reader output payload, retained through host observation.
  uint64_t output;
  // GPU-produced upload descriptor, acquired by the dispatch.
  uint64_t selection;
  // Reader-owned 32-bit start marker on a separate cache line.
  uint64_t started;
  // Positive number of input sweeps contributing to every output value.
  uint32_t round_count;
  // Reader ordinal, mixed into the exact arithmetic oracle.
  uint32_t reader;
};
static_assert(offsetof(ReaderArguments, round_count) == 32);

struct alignas(64) Selection {
  // Source page chosen from the token-like request.
  uint32_t page;
  // Cyclic input slot assigned by the immutable queue dependency schedule.
  uint32_t slot;
  // Number of words uploaded and consumed by every reader.
  uint32_t word_count;
  // Request mixed with this upload's generation.
  uint32_t hash;
  // Published SDMA frontier, distinct from consumed command storage.
  uint64_t frontier;
  // Prior job's reader start markers sampled before the copy.
  uint32_t started_before;
  // Prior job's incomplete native reader signals sampled after the copy.
  uint32_t pending_after;
  // Untouched suffix following the produced descriptor.
  uint32_t guards[8];
};
static_assert(sizeof(Selection) == 64);

}  // namespace kernels::device_sdma_lookahead

#endif  // AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_LOOKAHEAD_H_
