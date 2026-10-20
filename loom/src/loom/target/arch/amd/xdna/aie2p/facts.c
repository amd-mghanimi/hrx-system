// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/facts.h"

#include "loom/target/arch/amd/xdna/aie2p/records/target_records.h"
#include "loom/target/facts_builder.h"

static bool loom_aie2p_target_facts_satisfy_device_requirement(
    const loom_aie2p_target_facts_t* effective,
    const loom_aie2p_target_facts_t* requirement) {
  return requirement->device_profile == NULL ||
         effective->device_profile == requirement->device_profile;
}

static bool loom_aie2p_target_facts_satisfy_identity_requirement(
    const loom_target_facts_t* base_effective,
    const loom_target_facts_t* base_requirement) {
  const loom_aie2p_target_facts_t* effective =
      (const loom_aie2p_target_facts_t*)base_effective;
  const loom_aie2p_target_facts_t* requirement =
      (const loom_aie2p_target_facts_t*)base_requirement;
  return effective->base.selector == requirement->base.selector &&
         loom_aie2p_target_facts_satisfy_device_requirement(effective,
                                                            requirement);
}

static bool loom_aie2p_target_facts_satisfy_specialization_requirement(
    const loom_target_facts_t* base_effective,
    const loom_target_facts_t* base_requirement) {
  const loom_aie2p_target_facts_t* effective =
      (const loom_aie2p_target_facts_t*)base_effective;
  const loom_aie2p_target_facts_t* requirement =
      (const loom_aie2p_target_facts_t*)base_requirement;
  return loom_target_facts_structural_satisfy_specialization_requirement(
             base_effective, base_requirement) &&
         loom_aie2p_target_facts_satisfy_device_requirement(effective,
                                                            requirement);
}

static iree_string_view_t loom_aie2p_target_facts_identity_name(
    const loom_target_facts_t* base_facts) {
  const loom_aie2p_target_facts_t* facts =
      (const loom_aie2p_target_facts_t*)base_facts;
  return facts->device_profile != NULL
             ? iree_make_cstring_view(facts->device_profile->key)
             : facts->base.storage.bundle.name;
}

static iree_status_t loom_aie2p_target_facts_project_worker(
    const loom_target_facts_t* facts, iree_arena_allocator_t* arena,
    const loom_target_facts_t** out_facts) {
  *out_facts = facts;
  if (facts->selector == LOOM_AIE2P_TARGET_KIND_CORE) {
    return iree_ok_status();
  }
  loom_target_facts_t* worker = NULL;
  IREE_RETURN_IF_ERROR(loom_target_facts_builder_clone(facts, arena, &worker));
  loom_target_facts_builder_set_worker_contract(
      LOOM_AIE2P_TARGET_KIND_CORE, &loom_aie2p_core_target_bundle, worker);
  *out_facts = worker;
  return iree_ok_status();
}

const loom_target_fact_type_t loom_aie2p_target_fact_type = {
    .name = IREE_SVL("amd.xdna.aie2p"),
    .storage_size = sizeof(loom_aie2p_target_facts_t),
    .satisfies_identity_requirement =
        loom_aie2p_target_facts_satisfy_identity_requirement,
    .satisfies_specialization_requirement =
        loom_aie2p_target_facts_satisfy_specialization_requirement,
    .identity_name = loom_aie2p_target_facts_identity_name,
    .project_worker = loom_aie2p_target_facts_project_worker,
};
