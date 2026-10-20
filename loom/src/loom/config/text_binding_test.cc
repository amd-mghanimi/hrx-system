// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/config/text_binding.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class ConfigTextBindingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loom_config_text_binding_set_initialize(iree_allocator_system(),
                                            &binding_set_);
  }

  void TearDown() override {
    loom_config_text_binding_set_deinitialize(&binding_set_);
  }

  void ExpectBinding(iree_host_size_t index, iree_string_view_t expected_key,
                     iree_string_view_t expected_value) {
    ASSERT_LT(index, binding_set_.binding_count);
    EXPECT_TRUE(
        iree_string_view_equal(binding_set_.bindings[index].key, expected_key));
    EXPECT_TRUE(iree_string_view_equal(binding_set_.bindings[index].value,
                                       expected_value));
  }

  loom_config_text_binding_set_t binding_set_;
};

TEST_F(ConfigTextBindingTest, AppendsNormalizedOwnedBindings) {
  char key[] = "  @model.width ";
  char value[] = " 4096 ";
  IREE_ASSERT_OK(loom_config_text_binding_set_append(
      &binding_set_, iree_make_string_view(key, sizeof(key) - 1),
      iree_make_string_view(value, sizeof(value) - 1)));

  key[3] = 'x';
  value[1] = '8';
  ASSERT_EQ(binding_set_.binding_count, 1u);
  ExpectBinding(0, IREE_SV("model.width"), IREE_SV("4096"));
}

TEST_F(ConfigTextBindingTest, RejectsInvalidAndDuplicateBindings) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        loom_config_text_binding_set_append(
                            &binding_set_, IREE_SV(" @ "), IREE_SV("1")));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_config_text_binding_set_append(&binding_set_, IREE_SV("model.width"),
                                          IREE_SV("  ")));
  IREE_ASSERT_OK(loom_config_text_binding_set_append(
      &binding_set_, IREE_SV("@model.width"), IREE_SV("4096")));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_config_text_binding_set_append(
          &binding_set_, IREE_SV(" model.width "), IREE_SV("8192")));
  EXPECT_EQ(binding_set_.binding_count, 1u);
}

TEST_F(ConfigTextBindingTest, FlattensJsonObjectLeaves) {
  IREE_ASSERT_OK(loom_config_text_binding_set_append_json_object(&binding_set_,
                                                                 IREE_SV(R"({
        // JSONC comments are accepted for hand-authored configuration.
        "model": {
          "width": 4096,
          "enabled": true
        },
        "@direct": "4\u0032",
        "looks_object": "{not_object}"
      })")));

  ASSERT_EQ(binding_set_.binding_count, 4u);
  ExpectBinding(0, IREE_SV("model.width"), IREE_SV("4096"));
  ExpectBinding(1, IREE_SV("model.enabled"), IREE_SV("true"));
  ExpectBinding(2, IREE_SV("direct"), IREE_SV("42"));
  ExpectBinding(3, IREE_SV("looks_object"), IREE_SV("{not_object}"));
}

TEST_F(ConfigTextBindingTest, RejectsUnsupportedJsonValues) {
  const iree_string_view_t json_objects[] = {
      IREE_SV(R"({"items": [1]})"),
      IREE_SV(R"({"missing": null})"),
      IREE_SV(R"({"": 1})"),
      IREE_SV(R"({"value": 1} trailing)"),
  };
  for (iree_string_view_t json_object : json_objects) {
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          loom_config_text_binding_set_append_json_object(
                              &binding_set_, json_object));
    loom_config_text_binding_set_deinitialize(&binding_set_);
    loom_config_text_binding_set_initialize(iree_allocator_system(),
                                            &binding_set_);
  }
}

TEST_F(ConfigTextBindingTest, RejectsDuplicateFlattenedJsonKeys) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        loom_config_text_binding_set_append_json_object(
                            &binding_set_, IREE_SV(R"({"model.width": 4096,
                                      "model": {"width": 8192}})")));
}

}  // namespace
}  // namespace loom
