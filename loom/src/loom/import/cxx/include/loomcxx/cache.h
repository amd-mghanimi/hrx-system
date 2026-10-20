// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_CACHE_H_
#define LOOMCXX_CACHE_H_

namespace loom::cache {

// Logical execution scope at which an advisory cache policy applies. Targets
// map these portable domains to their physical cache hierarchy.
enum class scope : unsigned char {
  // Cache-policy scope is the current workgroup.
  workgroup = 0,
  // Cache-policy scope is the current workgroup cluster.
  cluster = 1,
  // Cache-policy scope is the current device.
  device = 2,
  // Cache-policy scope is the full system.
  system = 3,
};

// Temporal cache policy for memory operations. Policies are advisory and do
// not change memory ordering or coherence semantics.
enum class temporal : unsigned char {
  // Regular temporal caching behavior.
  regular = 0,
  // Data is expected to have little or no temporal reuse.
  non_temporal = 1,
  // Data is expected to be reused and retained when possible.
  high_temporal = 2,
  // Data is not expected to be used again after this operation.
  last_use = 3,
  // Store-oriented high-temporal write-back policy.
  writeback = 4,
  // Non-temporal near-cache and regular outer-cache behavior.
  non_temporal_regular = 5,
  // Regular near-cache and non-temporal outer-cache behavior.
  regular_non_temporal = 6,
  // Non-temporal near-cache and high-temporal outer-cache behavior.
  non_temporal_high_temporal = 7,
  // Non-temporal near-cache and write-back outer-cache behavior.
  non_temporal_writeback = 8,
  // Bypass caches at Scope when the target supports it.
  bypass = 9,
};

}  // namespace loom::cache

#endif  // LOOMCXX_CACHE_H_
