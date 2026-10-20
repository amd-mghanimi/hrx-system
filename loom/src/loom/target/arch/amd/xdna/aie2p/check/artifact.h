// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_CHECK_ARTIFACT_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_CHECK_ARTIFACT_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compiles authored programs and projects their native ELF resource contracts.
extern const loom_check_emit_provider_t loom_aie2p_artifact_check_emit_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_CHECK_ARTIFACT_H_
