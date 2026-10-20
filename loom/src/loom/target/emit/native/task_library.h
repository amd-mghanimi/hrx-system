// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Native task HAL library data contributions.

#ifndef LOOM_TARGET_EMIT_NATIVE_TASK_LIBRARY_H_
#define LOOM_TARGET_EMIT_NATIVE_TASK_LIBRARY_H_

#include "iree/base/internal/arena.h"
#include "loom/target/abi/task/parameter_layout.h"
#include "loom/target/emit/native/object.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_native_task_library_entry_t {
  // Export name, borrowed until the contribution has been built.
  iree_string_view_t name;
  // Native symbol index of the prepared dispatch function.
  iree_host_size_t symbol_index;
  // Dispatch requirements and logical parameter locations.
  loom_task_parameter_layout_t abi;
} loom_native_task_library_entry_t;

typedef struct loom_native_task_library_data_t {
  // Arena-owned readonly data, independent of the input records.
  loom_native_section_contribution_t section;
  // Arena-owned pointer fixups referencing the caller's native symbol table.
  loom_native_object_fixup_t* fixups;
  // Number of populated pointer fixups.
  iree_host_size_t fixup_count;
} loom_native_task_library_data_t;

// Serializes the versioned 64-bit little-endian task library layout. Every
// pointer is represented by a native fixup; no compiler-host address is copied
// into the image. Entry preparation owns signature, count, and export-name
// validation. The caller supplies the symbol/section indices reserved for the
// library data and the target relocation kind for a pointer. No target
// instruction is inspected.
iree_status_t loom_native_task_library_build_64le(
    iree_string_view_t name, const loom_native_task_library_entry_t* entries,
    uint16_t entry_count, iree_host_size_t library_symbol_index,
    iree_host_size_t section_index, uint32_t pointer_relocation_kind,
    iree_arena_allocator_t* arena, loom_native_task_library_data_t* out_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_TASK_LIBRARY_H_
