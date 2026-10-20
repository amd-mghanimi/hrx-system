// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_BATCHED_H_
#define AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_BATCHED_H_

#include <cstdint>

namespace kernels::device_sdma_batched {

struct alignas(16) Arguments {
  // GPU view of the SDMA command ring.
  uint64_t ring;
  // Aligned 64-bit command consumption frontier in bytes.
  uint64_t read_index;
  // Aligned 64-bit published frontier in bytes.
  uint64_t write_index;
  // Write-only, aligned 64-bit GPU notification address.
  uint64_t notification;
  // Credit-count destination slots, each 512 bytes with payload at byte 64.
  uint64_t destinations;
  // Separate 64-byte-aligned monotonic SDMA completion generation.
  uint64_t completion;
  // Aligned transcript with 80 words per transfer, initially guard-filled.
  uint64_t records;
  // Aligned 12-word admission, publication and terminal-state result block.
  uint64_t statistics;
  // Eight 512-byte source pages with 256-byte payloads at byte 64.
  uint64_t source_address;
  // Numeric COPY base for the same allocation as destinations.
  uint64_t destination_address;
  // Numeric FENCE address for the same word as completion.
  uint64_t completion_address;
  // Power-of-two command-ring byte capacity in [4096, 2^32].
  uint64_t capacity;
  // Finite transfer count below generation wrap; zero performs no publication.
  uint32_t round_count;
  // Nonzero batch size; later batches depend on earlier copied payloads.
  uint32_t batch_size;
  // Power-of-two payload-slot count in [1, 128], independent of ring capacity.
  uint32_t credit_count;
  // Initial batch state; subsequent states accumulate actual copied values.
  uint32_t seed;
  // Family-selected scope fields for COPY_LINEAR DWORD 2.
  uint32_t copy_control;
  // Family-selected complete FENCE header.
  uint32_t fence_header;
  // Queried USER_GCR acquire (bit 0) and release (bit 1) requirements.
  uint32_t cache_flags;
};

}  // namespace kernels::device_sdma_batched

#endif  // AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_BATCHED_H_
