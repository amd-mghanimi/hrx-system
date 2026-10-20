// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_CONFIGURATION_STORAGE_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_CONFIGURATION_STORAGE_H_

#include "loom/analysis/source_storage_packing.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/tile_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Joins a compiled worker's data domains to the owning tile's packing. The
// packing already excludes every configuration-owned data.reserve range and
// retains placements of earlier workers. Code remains in instruction memory;
// private, workgroup, scratch, stack and read-only domains share data memory.
// Spills are already included in their owning domain and are not counted twice.
//
// The contribution is immutable and may be linked at other coordinates with
// different reservations. All layout storage belongs to arena. A capacity
// rejection sets out_fits false; the packing retains the rejected requirement
// for diagnostics and the caller abandons the entry. Status carries allocation
// and target address-formation failures. No native bytes are emitted here.
iree_status_t loom_aie2p_configuration_storage_place(
    const loom_aie2p_leaf_contribution_t* contribution,
    const loom_xdna_array_family_t* family,
    loom_xdna_tile_coordinate_t coordinate,
    loom_source_storage_packing_t* packing, iree_arena_allocator_t* arena,
    loom_aie2p_tile_link_layout_t* out_layout, bool* out_fits);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_CONFIGURATION_STORAGE_H_
