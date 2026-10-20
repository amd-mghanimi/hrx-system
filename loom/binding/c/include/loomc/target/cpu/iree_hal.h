// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_TARGET_CPU_IREE_HAL_H_
#define LOOMC_TARGET_CPU_IREE_HAL_H_

#include "loomc/target/iree_hal.h"

/// @file
/// Native CPU target selection for IREE HAL devices.
///
/// The provider projects the standard CPU device-spec facet into the native
/// Loom target profile selected by the linked target environment. It pairs
/// that profile with the device's generic CPU executable target so compilation
/// and loading use one coherent selection.

#ifdef __cplusplus
extern "C" {
#endif

/// Returns the generic IREE HAL router provider for native CPU devices.
///
/// @return Process-lifetime provider descriptor. The returned pointer is
/// immutable and may be placed directly in a
/// `loomc_iree_hal_target_options_t::providers` array.
LOOMC_API_EXPORT const loomc_iree_hal_target_provider_t*
loomc_cpu_iree_hal_target_provider(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_TARGET_CPU_IREE_HAL_H_
