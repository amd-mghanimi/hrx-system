// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_RESIDENT_MEMORY_H_
#define AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_RESIDENT_MEMORY_H_

#include <array>
#include <span>
#include <vector>

#include "libamdf/cts/interop/gpu/xdna/recipes/device_fixture.h"

enum class ResidentSourceOffset { kZero, kAligned };

// Cold construction facts for one explicit GPU export and NPU import pair.
struct ResidentImportPlan {
  struct Endpoint {
    // Exact scope profile for this endpoint's construction role.
    amdf_memory_profile_t profile = {};
    // Exact transport row, including the provider's opaque provenance.
    amdf_external_memory_support_t transport = {};
    // Borrowed device and the complete required access set.
    amdf_memory_device_access_t access = {};
  };
  // GPU source allocation, export and sole public host mapping.
  Endpoint source;
  // Independently owned XDNA attachment, without a public host mapping.
  Endpoint destination;
  // Logical source offset of the exported range, excluding native prefixes.
  uint64_t source_byte_offset = 0;
  // Common granularity for the complete exported logical extent.
  uint64_t byte_length_granularity = 0;
  // Required device address alignment for the imported logical base.
  uint64_t minimum_alignment = 0;

  // A missing construction/transport records a capability skip before work.
  void Find(const amdf_api_t* api, amdf_memory_scope_t* scope,
            const amdf_memory_device_access_t& gpu_access,
            const amdf_memory_device_access_t& xdna_access,
            amdf_external_memory_type_t transport,
            ResidentSourceOffset source_offset);
};

// Protocol storage with a complete backing oracle and a logical subrange view.
// All native uses must retire before explicit release; destruction never
// implies cancellation or hides a failed release.
struct ResidentBuffer {
  void CreateImported(const amdf_api_t* api, amdf_memory_scope_t* scope,
                      const ResidentImportPlan& plan,
                      uint64_t minimum_byte_length);
  void QueryImportedPairs(const amdf_api_t* api, uint32_t gpu_family,
                          uint32_t xdna_family);
  bool Release(const amdf_api_t* api);

  std::span<uint8_t> bytes() const {
    return memory.bytes().subspan(logical.byte_offset, logical.byte_length);
  }
  std::span<uint8_t> expected() {
    return std::span(expected_backing)
        .subspan(logical.byte_offset, logical.byte_length);
  }

  // Host-only source retained until a joint registration is removed.
  CtsMappedMemory registration;
  // Joint owner or GPU-only source, with the sole protocol CPU view.
  CtsMappedMemory memory;
  // Separate imported XDNA owner; absent for joint backing.
  amdf_memory_t* xdna_import = nullptr;
  // Move-owned export, empty after successful import or explicit release.
  amdf_external_memory_t external = {};
  // Coordinates within the complete mapped GPU or joint logical allocation.
  struct {
    // Start of the shared range, before the protocol's leading guard.
    uint64_t byte_offset = 0;
    // Complete shared extent, including protocol guards and padding.
    uint64_t byte_length = 0;
  } logical;
  // GPU address of the payload, after the source offset and leading guard.
  uint64_t gpu_address = 0;
  // Imported or joint NPU DMA address of that same payload byte.
  uint64_t npu_address = 0;
  // Complete mapped-source oracle, including unexported prefix and suffix.
  std::vector<uint8_t> expected_backing;
  // Concrete directional visibility facts for this buffer's exact owners.
  std::array<amdf_memory_pair_info_t, kGpuXdnaJointEdges.size()> pairs = {};
};

#endif  // AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_RESIDENT_MEMORY_H_
