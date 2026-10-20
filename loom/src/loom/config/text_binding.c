// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/config/text_binding.h"

#include <string.h>

#include "iree/base/api.h"
#include "iree/base/internal/json.h"

void loom_config_text_binding_set_initialize(
    iree_allocator_t host_allocator,
    loom_config_text_binding_set_t* out_text_binding_set) {
  IREE_ASSERT_ARGUMENT(out_text_binding_set);
  memset(out_text_binding_set, 0, sizeof(*out_text_binding_set));
  out_text_binding_set->host_allocator = host_allocator;
}

void loom_config_text_binding_set_deinitialize(
    loom_config_text_binding_set_t* text_binding_set) {
  if (!text_binding_set) {
    return;
  }
  for (iree_host_size_t i = 0; i < text_binding_set->binding_count; ++i) {
    iree_allocator_free(text_binding_set->host_allocator,
                        (void*)text_binding_set->bindings[i].key.data);
    iree_allocator_free(text_binding_set->host_allocator,
                        (void*)text_binding_set->bindings[i].value.data);
  }
  iree_allocator_free(text_binding_set->host_allocator,
                      text_binding_set->bindings);
  iree_allocator_t host_allocator = text_binding_set->host_allocator;
  memset(text_binding_set, 0, sizeof(*text_binding_set));
  text_binding_set->host_allocator = host_allocator;
}

iree_string_view_t loom_config_text_binding_normalize_key(
    iree_string_view_t key) {
  key = iree_string_view_trim(key);
  (void)iree_string_view_consume_prefix_char(&key, '@');
  return iree_string_view_trim(key);
}

static iree_status_t loom_config_text_binding_clone_string_view(
    iree_allocator_t allocator, iree_string_view_t value,
    iree_string_view_t* out_value) {
  iree_host_size_t byte_length = 0;
  if (!iree_host_size_checked_add(value.size, 1, &byte_length)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "config string length overflow");
  }
  char* data = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, byte_length, (void**)&data));
  memcpy(data, value.data, value.size);
  data[value.size] = '\0';
  *out_value = iree_make_string_view(data, value.size);
  return iree_ok_status();
}

static iree_status_t loom_config_text_binding_set_reserve(
    loom_config_text_binding_set_t* text_binding_set,
    iree_host_size_t minimum_capacity) {
  if (text_binding_set->binding_capacity >= minimum_capacity) {
    return iree_ok_status();
  }
  iree_host_size_t new_capacity = text_binding_set->binding_capacity
                                      ? text_binding_set->binding_capacity
                                      : 4;
  while (new_capacity < minimum_capacity) {
    if (new_capacity > IREE_HOST_SIZE_MAX / 2) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "config binding capacity overflow");
    }
    new_capacity *= 2;
  }
  IREE_RETURN_IF_ERROR(iree_allocator_realloc_array(
      text_binding_set->host_allocator, new_capacity,
      sizeof(*text_binding_set->bindings),
      (void**)&text_binding_set->bindings));
  text_binding_set->binding_capacity = new_capacity;
  return iree_ok_status();
}

static iree_status_t loom_config_text_binding_set_check_duplicate(
    const loom_config_text_binding_set_t* text_binding_set,
    iree_string_view_t key) {
  for (iree_host_size_t i = 0; i < text_binding_set->binding_count; ++i) {
    if (!iree_string_view_equal(text_binding_set->bindings[i].key, key)) {
      continue;
    }
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "duplicate config binding for '%.*s'",
                            (int)key.size, key.data);
  }
  return iree_ok_status();
}

iree_status_t loom_config_text_binding_set_append(
    loom_config_text_binding_set_t* text_binding_set, iree_string_view_t key,
    iree_string_view_t value) {
  IREE_ASSERT_ARGUMENT(text_binding_set);
  key = loom_config_text_binding_normalize_key(key);
  value = iree_string_view_trim(value);
  if (iree_string_view_is_empty(key)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "config binding key must not be empty");
  }
  if (iree_string_view_is_empty(value)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "config binding for '%.*s' must provide a non-empty value",
        (int)key.size, key.data);
  }
  IREE_RETURN_IF_ERROR(
      loom_config_text_binding_set_check_duplicate(text_binding_set, key));
  iree_host_size_t minimum_capacity = 0;
  if (!iree_host_size_checked_add(text_binding_set->binding_count, 1,
                                  &minimum_capacity)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "config binding count overflow");
  }
  IREE_RETURN_IF_ERROR(
      loom_config_text_binding_set_reserve(text_binding_set, minimum_capacity));

  iree_string_view_t copied_key = iree_string_view_empty();
  iree_status_t status = loom_config_text_binding_clone_string_view(
      text_binding_set->host_allocator, key, &copied_key);
  iree_string_view_t copied_value = iree_string_view_empty();
  if (iree_status_is_ok(status)) {
    status = loom_config_text_binding_clone_string_view(
        text_binding_set->host_allocator, value, &copied_value);
  }
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(text_binding_set->host_allocator,
                        (void*)copied_key.data);
    return status;
  }

  text_binding_set->bindings[text_binding_set->binding_count++] =
      (loom_config_text_binding_t){
          .key = copied_key,
          .value = copied_value,
      };
  return iree_ok_status();
}

static iree_status_t loom_config_text_binding_append_unescaped_json_string(
    iree_string_view_t escaped_string, iree_string_builder_t* builder,
    iree_host_size_t* out_length) {
  iree_host_size_t unescaped_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_json_unescape_string(escaped_string, 0, NULL, &unescaped_length));
  char* target = NULL;
  if (unescaped_length > 0) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_inline(builder, unescaped_length, &target));
    IREE_RETURN_IF_ERROR(iree_json_unescape_string(
        escaped_string, unescaped_length, target, &unescaped_length));
  }
  *out_length = unescaped_length;
  return iree_ok_status();
}

static iree_status_t loom_config_text_binding_append_json_key(
    iree_string_view_t prefix, iree_string_view_t escaped_key,
    iree_string_builder_t* builder) {
  if (!iree_string_view_is_empty(prefix)) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_string(builder, prefix));
    IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "."));
  }
  iree_host_size_t key_length = 0;
  IREE_RETURN_IF_ERROR(loom_config_text_binding_append_unescaped_json_string(
      escaped_key, builder, &key_length));
  if (key_length == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "config JSON object keys must not be empty");
  }
  return iree_ok_status();
}

static iree_status_t loom_config_text_binding_set_append_json_value(
    loom_config_text_binding_set_t* text_binding_set, iree_string_view_t key,
    iree_json_value_type_t value_type, iree_string_view_t value) {
  switch (value_type) {
    case IREE_JSON_VALUE_TYPE_STRING: {
      iree_string_builder_t value_builder;
      iree_string_builder_initialize(text_binding_set->host_allocator,
                                     &value_builder);
      iree_host_size_t value_length = 0;
      iree_status_t status =
          loom_config_text_binding_append_unescaped_json_string(
              value, &value_builder, &value_length);
      if (iree_status_is_ok(status)) {
        status = loom_config_text_binding_set_append(
            text_binding_set, key, iree_string_builder_view(&value_builder));
      }
      iree_string_builder_deinitialize(&value_builder);
      return status;
    }
    case IREE_JSON_VALUE_TYPE_NUMBER:
    case IREE_JSON_VALUE_TYPE_TRUE:
    case IREE_JSON_VALUE_TYPE_FALSE:
      return loom_config_text_binding_set_append(text_binding_set, key, value);
    case IREE_JSON_VALUE_TYPE_ARRAY:
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "config JSON binding '%.*s' uses an unsupported array value",
          (int)key.size, key.data);
    case IREE_JSON_VALUE_TYPE_NULL:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "config JSON binding '%.*s' must not be null",
                              (int)key.size, key.data);
    case IREE_JSON_VALUE_TYPE_OBJECT:
    default:
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "config JSON binding '%.*s' has unsupported value type",
          (int)key.size, key.data);
  }
}

typedef struct loom_config_text_binding_json_object_state_t {
  loom_config_text_binding_set_t* text_binding_set;
  iree_string_view_t prefix;
  uint8_t depth;
} loom_config_text_binding_json_object_state_t;

static iree_status_t loom_config_text_binding_set_append_json_object_impl(
    loom_config_text_binding_set_t* text_binding_set, iree_string_view_t object,
    iree_string_view_t prefix, uint8_t depth);

static iree_status_t loom_config_text_binding_set_append_json_member(
    void* user_data, iree_string_view_t escaped_key,
    iree_json_value_type_t value_type, iree_string_view_t value) {
  loom_config_text_binding_json_object_state_t* state =
      (loom_config_text_binding_json_object_state_t*)user_data;
  iree_string_builder_t key_builder;
  iree_string_builder_initialize(state->text_binding_set->host_allocator,
                                 &key_builder);
  iree_status_t status = loom_config_text_binding_append_json_key(
      state->prefix, escaped_key, &key_builder);
  if (iree_status_is_ok(status)) {
    iree_string_view_t key = iree_string_builder_view(&key_builder);
    if (value_type == IREE_JSON_VALUE_TYPE_OBJECT) {
      status = loom_config_text_binding_set_append_json_object_impl(
          state->text_binding_set, value, key, (uint8_t)(state->depth + 1));
    } else {
      status = loom_config_text_binding_set_append_json_value(
          state->text_binding_set, key, value_type, value);
    }
  }
  iree_string_builder_deinitialize(&key_builder);
  return status;
}

static iree_status_t loom_config_text_binding_set_append_json_object_impl(
    loom_config_text_binding_set_t* text_binding_set, iree_string_view_t object,
    iree_string_view_t prefix, uint8_t depth) {
  if (depth >= 128) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "config JSON object nesting is too deep");
  }
  loom_config_text_binding_json_object_state_t state = {
      .text_binding_set = text_binding_set,
      .prefix = prefix,
      .depth = depth,
  };
  return iree_json_enumerate_object_typed(
      object, loom_config_text_binding_set_append_json_member, &state);
}

iree_status_t loom_config_text_binding_set_append_json_object(
    loom_config_text_binding_set_t* text_binding_set,
    iree_string_view_t json_object) {
  IREE_ASSERT_ARGUMENT(text_binding_set);
  iree_string_view_t cursor = json_object;
  iree_string_view_t object = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_consume_object(&cursor, &object));
  IREE_RETURN_IF_ERROR(iree_json_consume_insignificant(&cursor));
  if (!iree_string_view_is_empty(cursor)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unexpected trailing content after config JSON");
  }
  return loom_config_text_binding_set_append_json_object_impl(
      text_binding_set, object, iree_string_view_empty(), /*depth=*/0);
}
