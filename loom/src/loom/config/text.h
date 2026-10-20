// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Textual configuration value materialization into Loom IR.

#ifndef LOOM_CONFIG_TEXT_H_
#define LOOM_CONFIG_TEXT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/config/application.h"
#include "loom/config/text_binding.h"
#include "loom/ir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Options for materializing textual configuration values into a module.
typedef struct loom_config_text_materialize_options_t {
  // Borrowed bindings for the current compiler operation. NULL is accepted and
  // treated as an empty set.
  const loom_config_text_binding_set_t* binding_set;
  // Observes applied values, excluding ignored caller bindings.
  loom_config_applied_value_sink_t applied_value_sink;
} loom_config_text_materialize_options_t;

// Initializes options to an empty binding set with no applied-value observer.
void loom_config_text_materialize_options_initialize(
    loom_config_text_materialize_options_t* out_options);

// Replaces matching config.decl/config.def symbols in |module| with config.def
// operations whose initializer attributes are parsed from
// |options->binding_set|. Bindings without matching configuration symbols are
// ignored.
iree_status_t loom_config_text_materialize_module(
    loom_module_t* module,
    const loom_config_text_materialize_options_t* options,
    iree_arena_block_pool_t* block_pool,
    loom_config_application_result_t* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CONFIG_TEXT_H_
