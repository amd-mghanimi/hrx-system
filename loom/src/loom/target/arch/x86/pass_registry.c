// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/x86/pass_registry.h"

#include "loom/codegen/low/pipeline/pass_environment.h"
#include "loom/target/arch/x86/hal_materialization.h"
#include "loom/target/pass_environment.h"
#include "loom/target/pass_requirements.h"

static const loom_pass_info_t* loom_x86_hal_kernel_pass_info(void) {
  static const loom_pass_info_t info = {
      .name = IREE_SVL("x86-materialize-hal-kernel"),
      .description =
          IREE_SVL("Materialize the physical task dispatch entry ABI."),
      .kind = LOOM_PASS_FUNCTION,
  };
  return &info;
}

static const loom_pass_info_t* loom_x86_hal_query_pass_info(void) {
  static const loom_pass_info_t info = {
      .name = IREE_SVL("x86-materialize-hal-query"),
      .description = IREE_SVL(
          "Materialize the task library query and its compiler version."),
      .kind = LOOM_PASS_MODULE,
  };
  return &info;
}

static const loom_pass_requirement_def_t kKernelRequirements[] = {
    {.capability_type = &loom_low_pass_capability_type,
     .key = IREE_SVL(LOOM_LOW_PASS_REQUIREMENT_TARGET_LOW_DESCRIPTOR_REGISTRY),
     .description =
         IREE_SVL("Requires the selected Low instruction vocabulary.")},
};

static const loom_pass_requirement_def_t kQueryRequirements[] = {
    {.capability_type = &loom_low_pass_capability_type,
     .key = IREE_SVL(LOOM_LOW_PASS_REQUIREMENT_TARGET_LOW_DESCRIPTOR_REGISTRY),
     .description =
         IREE_SVL("Requires the selected Low instruction vocabulary.")},
    {.capability_type = &loom_target_pass_capability_type,
     .key = IREE_SVL(LOOM_TARGET_PASS_REQUIREMENT_MUTABLE_FUNCTION_VERSIONS),
     .description =
         IREE_SVL("Requires ownership of newly created function versions.")},
};

static const loom_pass_descriptor_t kDescriptors[] = {
    {.key = IREE_SVL("x86-materialize-hal-kernel"),
     .info = loom_x86_hal_kernel_pass_info,
     .function_run = loom_x86_materialize_hal_kernel_run,
     .requirement_defs = kKernelRequirements,
     .requirement_count = IREE_ARRAYSIZE(kKernelRequirements)},
    {.key = IREE_SVL("x86-materialize-hal-query"),
     .info = loom_x86_hal_query_pass_info,
     .module_run = loom_x86_materialize_hal_query_run,
     .requirement_defs = kQueryRequirements,
     .requirement_count = IREE_ARRAYSIZE(kQueryRequirements)},
};

const loom_pass_registry_t loom_x86_pass_registry = {
    .descriptors = kDescriptors,
    .descriptor_count = IREE_ARRAYSIZE(kDescriptors),
};
