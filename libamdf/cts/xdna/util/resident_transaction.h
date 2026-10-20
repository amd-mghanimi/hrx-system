// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_XDNA_UTIL_RESIDENT_TRANSACTION_H_
#define AMDF_CTS_XDNA_UTIL_RESIDENT_TRANSACTION_H_

#include <cstdint>
#include <span>
#include <vector>

#include "gtest/gtest.h"

// Paired storage with one writer per direction. Either program may initiate
// the exchange; request and response addresses always follow physical writers.
struct ResidentNpuSlot {
  // Four-byte peer-produced request generation, preceding its payload.
  uint64_t request_generation_address;
  // Complete peer-produced request payload.
  uint64_t request_payload_address;
  // Complete NPU-produced response payload.
  uint64_t response_payload_address;
  // Four-byte NPU-produced response generation, preceding its payload.
  uint64_t response_generation_address;
};

// External records used by one column's scalar-stream service. Each address
// comes from the corresponding NPU access query. The caller retains these
// nonoverlapping extents through command completion and native teardown.
struct ResidentNpuAddresses {
  // Four-byte host RUN/ABORT record in its own allocation and maintenance
  // range.
  uint64_t startup_address;
  // One or two paired slots, borrowed only during command construction.
  std::span<const ResidentNpuSlot> slots;
  // Four-byte peer acknowledgement after its final response reads.
  uint64_t final_ack_address;
};

// The final acknowledgement has its own line in the control allocation.
inline constexpr uint32_t kResidentFinalAckByteOffset = 192;

// Complete response DMA ownership for the corresponding authored service.
enum class ResidentResponsePath {
  // Payload and ready form one S2MM1 descriptor chain.
  kChained,
  // S2MM0 writes payload; S2MM1 acquires its lock before writing ready.
  kPayload0Ready1,
  // S2MM1 writes payload; S2MM0 acquires its lock before writing ready.
  kPayload1Ready0,
};

// Builds one native transaction around an already loaded and bound establishing
// invocation. The admitted compiler product owns configuration MM2S0, its
// terminal output's S2MM0, shim BDs 0/1, compute BDs 0..3 and vertical lane 0.
// The service uses direct Core0 scalar streams, shim MM2S1, BDs 2..5 per first
// slot and 6..9 per second slot, startup BD10, final ACK BD15, vertical lane 1
// and shim packet arbiter 1. Chained responses use S2MM1. Split responses own
// both S2MM channels and require the compiler terminal output in another
// column. They leave payload and ready as independent tasks, ordered by shim
// lock0; the immutable worker configuration chooses which task is armed first.
//
// The worker consumes a fresh final peer ACK, ceases custom submissions, then
// writes its ordinary terminal record before returning. ABORT writes that same
// terminal record without submitting any request/response task. The compiler's
// terminal S2MM0 token in the terminal output's column must precede the
// appended idle polls for MM2S1 and every service-owned S2MM channel.
//
// This is cold command construction, not publication or submission. Invocation
// records are copied unchanged after binding; only the outer header's size and
// operation count change. The caller supplies an inactive placement, retains
// every addressed owner and separately publishes the resulting command bytes.
// Address/header rejection leaves output unchanged, including when invocation
// borrows its old contents. No compiler container parsing is performed here.
// Payload length is a nonzero multiple of four bytes and matches the immutable
// service configuration's word count. Slot count matches its credit count.
// Services occupy consecutive logical columns starting at zero within the
// invocation's complete partition. Each service column owns its descriptors,
// routes and final DMA drain. Additional columns and their completion paths
// belong entirely to the compiler invocation.
::testing::AssertionResult BuildResidentTransaction(
    std::span<const uint8_t> invocation,
    std::span<const ResidentNpuAddresses> services,
    uint32_t payload_byte_length, ResidentResponsePath response_path,
    std::vector<uint8_t>* output);

#endif  // AMDF_CTS_XDNA_UTIL_RESIDENT_TRANSACTION_H_
