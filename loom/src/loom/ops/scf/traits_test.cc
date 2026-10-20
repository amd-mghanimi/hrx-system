// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/scf/ops.h"

namespace loom {
namespace {

class ScfTraitsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const auto* tables = loom_scf_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(&context_, LOOM_DIALECT_SCF,
                                                 tables, (uint16_t)count));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("traits"), &pool_,
                                        nullptr, iree_allocator_system(),
                                        &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  loom_value_id_t Input(loom_type_t type) {
    loom_value_id_t value = LOOM_VALUE_ID_INVALID;
    IREE_CHECK_OK(loom_builder_define_block_arg(
        &builder_, loom_module_block(module_), type, &value));
    return value;
  }

  loom_op_t* BuildSelect(loom_type_t payload_type) {
    const loom_value_id_t condition =
        Input(loom_type_scalar(LOOM_SCALAR_TYPE_I1));
    const loom_value_id_t true_value = Input(payload_type);
    const loom_value_id_t false_value = Input(payload_type);
    loom_op_t* select = nullptr;
    IREE_CHECK_OK(loom_scf_select_build(&builder_, condition, true_value,
                                        false_value, payload_type,
                                        LOOM_LOCATION_UNKNOWN, &select));
    return select;
  }

  iree_arena_block_pool_t pool_;
  loom_context_t context_;
  loom_module_t* module_ = nullptr;
  loom_builder_t builder_ = {};
};

TEST_F(ScfTraitsTest, DecomposableOnlyForVectorPayloads) {
  const loom_op_t* scalar = BuildSelect(loom_type_scalar(LOOM_SCALAR_TYPE_I32));
  EXPECT_FALSE(iree_any_bit_set(loom_op_effective_traits(module_, scalar),
                                LOOM_TRAIT_DECOMPOSABLE));

  const loom_op_t* vector = BuildSelect(
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I32, 32, 0));
  EXPECT_TRUE(iree_all_bits_set(loom_op_effective_traits(module_, vector),
                                LOOM_TRAIT_DECOMPOSABLE));

  const loom_op_t* tile = BuildSelect(
      loom_type_shaped_1d(LOOM_TYPE_TILE, LOOM_SCALAR_TYPE_I32, 32, 0));
  EXPECT_FALSE(iree_any_bit_set(loom_op_effective_traits(module_, tile),
                                LOOM_TRAIT_DECOMPOSABLE));
}

}  // namespace
}  // namespace loom
