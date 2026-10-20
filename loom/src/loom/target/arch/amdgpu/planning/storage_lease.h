// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU target storage leases for post-issue physical register hazards.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_STORAGE_LEASE_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_STORAGE_LEASE_H_

#include "iree/base/api.h"
#include "loom/codegen/low/storage_lease.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_amdgpu_xcnt_release_group_e {
  // Vector-memory packet sources retained by translation state.
  LOOM_AMDGPU_XCNT_RELEASE_GROUP_VMEM = 1,
  // Scalar-memory packet sources retained by translation state.
  LOOM_AMDGPU_XCNT_RELEASE_GROUP_SMEM = 2,
} loom_amdgpu_xcnt_release_group_t;

// Initializes |out_provider| with the stateless AMDGPU storage-lease provider.
void loom_amdgpu_storage_lease_provider(
    loom_low_storage_lease_provider_t* out_provider);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_STORAGE_LEASE_H_
