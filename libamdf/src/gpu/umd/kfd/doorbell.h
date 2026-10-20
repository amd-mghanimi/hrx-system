// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_DOORBELL_H_
#define AMDF_SRC_GPU_UMD_KFD_DOORBELL_H_

#include "libamdf/src/gpu/umd/kfd/device.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct amdf_gpu_kfd_doorbell_t amdf_gpu_kfd_doorbell_t;

// Creates an uncached GPU view of this device's process-owned doorbell slice.
// The target plan supplies the complete native slice length. The device is
// borrowed through destruction. Failure leaves both outputs unchanged and
// rolls back locally; failed rollback preserves unreleased backing and VA as
// leaks without a retained owner. The returned address is a GPU address only.
amdf_status_t amdf_gpu_kfd_doorbell_create(
    amdf_gpu_umd_device_t* device, size_t byte_length,
    amdf_gpu_kfd_doorbell_t** out_doorbell, uint64_t* out_device_address);

// Consumes the owner on every result, after all producers and its native queue
// have retired. Failed cleanup preserves unreleased backing and reserved VA;
// special-allocation release is never blindly retried.
amdf_status_t amdf_gpu_kfd_doorbell_destroy(amdf_gpu_kfd_doorbell_t* doorbell);

// Consumes metadata without native cleanup when queue retirement is unproven.
void amdf_gpu_kfd_doorbell_abandon(amdf_gpu_kfd_doorbell_t* doorbell);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_GPU_UMD_KFD_DOORBELL_H_
