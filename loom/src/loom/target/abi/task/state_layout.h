// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Versioned task invocation and library layouts for native target emission.
// Fixed-width geometry fields are independent of pointer width. ABI64 facts
// describe a 64-bit pointer layout; the compiler host does not select offsets.

#ifndef LOOM_TARGET_ABI_TASK_STATE_LAYOUT_H_
#define LOOM_TARGET_ABI_TASK_STATE_LAYOUT_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resident readonly library object returned by the compiled query function.
#define LOOM_TASK_LIBRARY_SYMBOL "iree_hal_executable_library_v0"

typedef enum loom_task_builtin_e {
  LOOM_TASK_BUILTIN_WORKGROUP_ID_X = 0,
  LOOM_TASK_BUILTIN_WORKGROUP_ID_Y,
  LOOM_TASK_BUILTIN_WORKGROUP_ID_Z,
  LOOM_TASK_BUILTIN_WORKGROUP_COUNT_X,
  LOOM_TASK_BUILTIN_WORKGROUP_COUNT_Y,
  LOOM_TASK_BUILTIN_WORKGROUP_COUNT_Z,
  LOOM_TASK_BUILTIN_COUNT_,
} loom_task_builtin_t;

typedef struct loom_task_builtin_info_t {
  // Qualified Low live-in source name for this invocation-constant value.
  iree_string_view_t name;
  // Physical state-pointer argument: one for dispatch, two for workgroup.
  uint8_t state_argument;
  // Byte offset in the state structure.
  uint8_t offset;
  // Unsigned field width in bytes, before widening to the index carrier.
  uint8_t size;
} loom_task_builtin_info_t;

// Dense semantic builtin identities shared by source and authored Low paths.
extern const loom_task_builtin_info_t
    loom_task_builtins[LOOM_TASK_BUILTIN_COUNT_];

// Field positions and sizes for the native 64-bit task ABI. Serialization uses
// explicit little-endian stores; these are target facts, never host offsetof.
enum {
  LOOM_TASK_ABI64_LIBRARY_SIZE = 112,
  LOOM_TASK_ABI64_LIBRARY_EXPORTS = 8,
  LOOM_TASK_ABI64_HEADER_SIZE = 24,
  LOOM_TASK_ABI64_HEADER_NAME = 8,
  LOOM_TASK_ABI64_EXPORT_POINTERS = 8,
  LOOM_TASK_ABI64_EXPORT_ATTRIBUTES = 16,
  LOOM_TASK_ABI64_EXPORT_PARAMETERS = 24,
  LOOM_TASK_ABI64_EXPORT_NAMES = 40,
  LOOM_TASK_ABI64_ATTRIBUTES_SIZE = 64,
  LOOM_TASK_ABI64_PARAMETER_SIZE = 8,
  LOOM_TASK_ABI64_POINTER_SIZE = 8,
  LOOM_TASK_ABI64_CONSTANTS_OFFSET = 24,
  LOOM_TASK_ABI64_BINDINGS_OFFSET = 40,
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ABI_TASK_STATE_LAYOUT_H_
