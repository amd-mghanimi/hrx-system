// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Structured JSON presentation of public LoomC diagnostics.

#ifndef LOOM_TOOLING_CLI_LOOMC_DIAGNOSTIC_JSON_H_
#define LOOM_TOOLING_CLI_LOOMC_DIAGNOSTIC_JSON_H_

#include "iree/base/api.h"
#include "loom/util/stream.h"
#include "loomc/result.h"

#ifdef __cplusplus
extern "C" {
#endif

// Writes one public compiler diagnostic using Loom's structured JSON schema.
iree_status_t loom_tooling_cli_write_loomc_diagnostic_json(
    loom_output_stream_t* stream, const loomc_diagnostic_t* diagnostic);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_CLI_LOOMC_DIAGNOSTIC_JSON_H_
