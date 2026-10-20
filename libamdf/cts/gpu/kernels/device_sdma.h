// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_H_
#define AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_H_

#include <cstdint>

namespace kernels::device_sdma {

struct alignas(16) Arguments {
  // GPU view of the SDMA command ring.
  uint64_t ring;
  // Aligned 64-bit command consumption frontier in bytes.
  uint64_t read_index;
  // Aligned 64-bit published frontier in bytes.
  uint64_t write_index;
  // Write-only, aligned 64-bit GPU notification address.
  uint64_t notification;
  // Four 512-byte destination slots, with 256-byte payloads at byte 64.
  uint64_t destinations;
  // Separate 64-byte-aligned SDMA completion generation, initially zero.
  uint64_t completion;
  // Base of the 64-byte-aligned transcript, with 72 words per generation.
  uint64_t records;
  // Eight 512-byte source pages used as numeric COPY operands.
  uint64_t source_address;
  // Numeric COPY base for the same allocation as destinations.
  uint64_t destination_address;
  // Numeric FENCE address for the same word as completion.
  uint64_t completion_address;
  // Power-of-two command-ring byte capacity in [4096, 2^32].
  uint64_t capacity;
  // Finite transfer count, leaving generation zero unused.
  uint32_t round_count;
  // Initial selection state; subsequent states derive from copied data.
  uint32_t seed;
  // Family-selected scope fields for COPY_LINEAR DWORD 2.
  uint32_t copy_control;
  // Family-selected complete FENCE header.
  uint32_t fence_header;
  // Queried USER_GCR acquire (bit 0) and release (bit 1) requirements.
  uint32_t cache_flags;
};

}  // namespace kernels::device_sdma

#endif  // AMDF_CTS_GPU_KERNELS_DEVICE_SDMA_H_
