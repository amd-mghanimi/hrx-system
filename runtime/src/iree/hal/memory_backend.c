// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory_backend.h"

IREE_API_EXPORT iree_status_t iree_hal_slab_pool_plan_create(
    iree_hal_slab_pool_plan_t* plan, iree_allocator_t host_allocator,
    iree_hal_pool_t** out_pool) {
  *out_pool = NULL;
  iree_status_t status = plan->vtable->create(plan, host_allocator, out_pool);
  iree_hal_slab_pool_plan_destroy(plan);
  return status;
}

IREE_API_EXPORT void iree_hal_slab_pool_plan_destroy(
    iree_hal_slab_pool_plan_t* plan) {
  if (plan) {
    plan->vtable->destroy(plan);
  }
}
