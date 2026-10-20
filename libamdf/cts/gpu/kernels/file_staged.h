// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_FILE_STAGED_H_
#define AMDF_CTS_GPU_KERNELS_FILE_STAGED_H_

#include <cstddef>
#include <cstdint>

namespace kernels::file_staged {

// File completion publishes these fields before the upload dispatch starts.
// Upload adds its frontier before independent readers can consume the copy.
struct alignas(64) Result {
  // GPU-selected block in the immutable private file.
  uint32_t block;
  // Requested payload length, in words.
  uint32_t word_count;
  // Token-like request mixed with this job's generation.
  uint32_t hash;
  // Zero on a complete read; otherwise the first negative terminal error.
  int32_t status;
  // Bytes actually written into the source slot, including a partial read.
  uint32_t bytes_read;
  // Native requests accepted for this job; zero after terminal failure.
  uint32_t request_count;
  // Prior source user's last reader remains pending until this read finishes.
  uint32_t previous_reader_pending;
  // Untouched word separating file-owned fields from the upload frontier.
  uint32_t guard;
  // Published SDMA command frontier, including no-op failed uploads.
  uint64_t frontier;
  // Untouched suffix of the descriptor.
  uint32_t guards[6];
};
static_assert(sizeof(Result) == 64);
static_assert(offsetof(Result, frontier) == 32);

struct alignas(16) ReadArguments {
  // Native SQE array, with reserved fields initially zero.
  uint64_t submission_entries;
  // GPU-owned SQ publication frontier.
  uint64_t submission_tail;
  // Native CQE array, consumed only by the file queue.
  uint64_t completion_entries;
  // GPU-owned CQ consumption frontier.
  uint64_t completion_head;
  // Kernel-owned CQ publication frontier.
  uint64_t completion_tail;
  // Persistent request ticket and terminal status, two 32-bit words.
  uint64_t state;
  // This job's retained result record.
  uint64_t result;
  // Immutable token-like input from which the GPU selects a file block.
  uint64_t request;
  // Previous source user's native last-reader value, or an idle sentinel.
  uint64_t previous_reader;
  // CPU virtual address of this source slot's registered payload.
  uint64_t host_payload;
  // Returned native SQ capacity minus one.
  uint32_t submission_mask;
  // Returned native CQ capacity minus one.
  uint32_t completion_mask;
  // Positive payload length in words, at most 16384.
  uint32_t word_count;
  // Registered private file ordinal; an invalid ordinal exercises error drain.
  uint32_t file_index;
  // Positive job ordinal, unique in the finite batch.
  uint32_t generation;
  // Number of immutable file blocks minus one, a power-of-two mask.
  uint32_t file_block_mask;
  // Zero-filled argument storage beyond the emitted ABI extent.
  uint32_t reserved[2];
};
static_assert(sizeof(ReadArguments) == 112);
static_assert(offsetof(ReadArguments, submission_mask) == 80);

struct alignas(16) UploadArguments {
  // GPU view of the byte-indexed SDMA ring.
  uint64_t ring;
  // Aligned 64-bit consumed command frontier.
  uint64_t read_index;
  // Aligned 64-bit published command frontier.
  uint64_t write_index;
  // Write-only aligned 64-bit queue notification address.
  uint64_t notification;
  // SDMA completion word, distinct from the command read frontier.
  uint64_t completion;
  // Persistent 64-bit frontier owned by the ordered upload queue.
  uint64_t state;
  // File result and retained upload frontier for this job.
  uint64_t result;
  // GPU address of the completed registered source payload.
  uint64_t source;
  // GPU address of the destination payload, after its guarded prefix.
  uint64_t destination;
  // Numeric SDMA address of the same word as completion.
  uint64_t completion_address;
  // Power-of-two SDMA ring byte capacity in [4096, 2^32].
  uint64_t capacity;
  // Positive job ordinal, unique in the finite batch.
  uint32_t generation;
  // Family-selected scope fields in COPY_LINEAR DWORD 2.
  uint32_t copy_control;
  // Family-selected complete FENCE header.
  uint32_t fence_header;
  // Queried USER_GCR acquire (bit 0) and release (bit 1).
  uint32_t cache_flags;
  // Zero-filled argument storage beyond the emitted ABI extent.
  uint32_t reserved[2];
};
static_assert(sizeof(UploadArguments) == 112);
static_assert(offsetof(UploadArguments, generation) == 88);

struct alignas(16) ReaderArguments {
  // Copied destination payload, retained until every reader completes.
  uint64_t input;
  // Per-job, per-reader output payload, retained for exact host verification.
  uint64_t output;
  // The completed file/upload result; failed jobs leave output untouched.
  uint64_t result;
  // Independent reader ordinal, mixed into every output word.
  uint32_t reader;
  // Zero-filled argument storage beyond the emitted ABI extent.
  uint32_t reserved;
};
static_assert(sizeof(ReaderArguments) == 32);

}  // namespace kernels::file_staged

#endif  // AMDF_CTS_GPU_KERNELS_FILE_STAGED_H_
