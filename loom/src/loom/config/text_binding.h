// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Owned textual configuration bindings for compiler operations.

#ifndef LOOM_CONFIG_TEXT_BINDING_H_
#define LOOM_CONFIG_TEXT_BINDING_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// One caller-provided textual configuration binding. Both strings are borrowed
// unless stored in an owning text binding set.
typedef struct loom_config_text_binding_t {
  // Normalized configuration symbol name without the textual '@' sigil.
  iree_string_view_t key;
  // Textual value parsed according to the matched config symbol type.
  iree_string_view_t value;
} loom_config_text_binding_t;

// Owned textual configuration bindings for one compiler operation.
//
// Text binding sets are explicit invocation state. They are intentionally
// separate from sessions, contexts, and pass managers so callers can reuse
// long-lived compiler infrastructure across independent compilations without
// ambient configuration leakage.
typedef struct loom_config_text_binding_set_t {
  // Host allocator owning the binding array and copied key/value strings.
  iree_allocator_t host_allocator;
  // Owned normalized bindings. Callers must not mutate this array directly.
  loom_config_text_binding_t* bindings;
  // Number of entries in |bindings|.
  iree_host_size_t binding_count;
  // Allocated capacity of |bindings|.
  iree_host_size_t binding_capacity;
} loom_config_text_binding_set_t;

// Normalizes a borrowed configuration key by trimming surrounding whitespace
// and removing one leading textual '@' sigil.
iree_string_view_t loom_config_text_binding_normalize_key(
    iree_string_view_t key);

// Initializes |out_text_binding_set| as an empty owned text binding set.
void loom_config_text_binding_set_initialize(
    iree_allocator_t host_allocator,
    loom_config_text_binding_set_t* out_text_binding_set);

// Releases all strings and storage owned by |text_binding_set|.
void loom_config_text_binding_set_deinitialize(
    loom_config_text_binding_set_t* text_binding_set);

// Appends one owned textual configuration binding to |text_binding_set|.
//
// The key is normalized with |loom_config_text_binding_normalize_key| and the
// value is trimmed. Duplicate normalized keys are rejected so precedence
// remains explicit.
iree_status_t loom_config_text_binding_set_append(
    loom_config_text_binding_set_t* text_binding_set, iree_string_view_t key,
    iree_string_view_t value);

// Parses a JSON/JSONC object and appends flattened textual configuration
// bindings.
//
// Nested object keys are joined with '.'. Leaf values may be JSON booleans,
// numbers, or strings. String leaves are unescaped and retained as text for
// later parsing against the matched configuration symbol type.
iree_status_t loom_config_text_binding_set_append_json_object(
    loom_config_text_binding_set_t* text_binding_set,
    iree_string_view_t json_object);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CONFIG_TEXT_BINDING_H_
