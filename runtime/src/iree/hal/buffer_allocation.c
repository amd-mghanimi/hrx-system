// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/buffer_allocation.h"

#include "iree/base/internal/atomics.h"

IREE_API_EXPORT uint64_t iree_hal_buffer_allocation_next_id(void) {
  static iree_atomic_int64_t next_id = IREE_ATOMIC_VAR_INIT(1);
  return (uint64_t)iree_atomic_fetch_add(&next_id, 1,
                                         iree_memory_order_relaxed);
}

IREE_API_EXPORT iree_hal_buffer_allocation_profile_t
iree_hal_buffer_allocation_profile(iree_hal_buffer_t* buffer) {
  return iree_hal_buffer_allocation_vtable(buffer)->profile(buffer);
}

IREE_API_EXPORT iree_status_t iree_hal_buffer_allocation_begin_dealloca(
    iree_hal_buffer_t* buffer, iree_hal_pool_t** out_pool) {
  return iree_hal_buffer_allocation_vtable(buffer)->begin_dealloca(buffer,
                                                                   out_pool);
}

IREE_API_EXPORT void iree_hal_buffer_allocation_abort_dealloca(
    iree_hal_buffer_t* buffer) {
  iree_hal_buffer_allocation_vtable(buffer)->abort_dealloca(buffer);
}

IREE_API_EXPORT void iree_hal_buffer_allocation_take_dealloca_reservation(
    iree_hal_buffer_t* buffer, iree_hal_pool_t** out_pool,
    iree_hal_pool_reservation_t* out_reservation) {
  iree_hal_buffer_allocation_vtable(buffer)->take_dealloca_reservation(
      buffer, out_pool, out_reservation);
}

IREE_API_EXPORT void iree_hal_buffer_allocation_decommit(
    iree_hal_buffer_t* buffer) {
  iree_hal_buffer_allocation_vtable(buffer)->decommit(buffer);
}
