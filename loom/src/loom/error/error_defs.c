// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/error/error_defs.h"

#include "loom/error/error_catalog.h"
#include "loom/error/error_defs_tables.inl"

static_assert(sizeof(loom_error_def_t) == (IREE_PTR_SIZE == 8 ? 32 : 28),
              "generated error definitions must remain compact");
static_assert(sizeof(loom_error_param_def_t) == 4,
              "generated error parameter definitions must remain packed");
static_assert(sizeof(loom_error_domain_span_t) == 4,
              "generated error domain spans must remain packed");

const char* loom_diagnostic_severity_name(loom_diagnostic_severity_t severity) {
  if (severity < IREE_ARRAYSIZE(loom_diagnostic_severity_names)) {
    const char* name = loom_diagnostic_severity_names[severity];
    if (name != NULL) {
      return name;
    }
  }
  return "unknown";
}

const char* loom_error_domain_name(loom_error_domain_t domain) {
  if (domain < IREE_ARRAYSIZE(loom_error_domain_names)) {
    const char* name = loom_error_domain_names[domain];
    if (name != NULL) {
      return name;
    }
  }
  return "UNKNOWN";
}

bool loom_error_domain_from_name(iree_string_view_t name,
                                 loom_error_domain_t* out_domain) {
  if (out_domain == NULL) {
    return false;
  }
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(loom_error_domain_names);
       ++i) {
    const char* domain_name = loom_error_domain_names[i];
    if (domain_name != NULL &&
        iree_string_view_equal(name, iree_make_cstring_view(domain_name))) {
      *out_domain = (loom_error_domain_t)i;
      return true;
    }
  }
  return false;
}

bool loom_error_ref_parse(iree_string_view_t value, loom_error_ref_t* out_ref) {
  if (out_ref == NULL) {
    return false;
  }
  *out_ref = LOOM_ERROR_REF_NONE;
  iree_string_view_t domain_name = iree_string_view_empty();
  iree_string_view_t code_text = iree_string_view_empty();
  loom_error_domain_t domain = LOOM_ERROR_DOMAIN_COUNT_;
  uint32_t code = 0;
  if (iree_string_view_split(value, '/', &domain_name, &code_text) < 0 ||
      !loom_error_domain_from_name(domain_name, &domain) ||
      !iree_string_view_atoi_uint32_base(code_text, 10, &code) || code == 0 ||
      code > LOOM_ERROR_REF_CODE_MASK) {
    return false;
  }
  char canonical_code[5] = {0};
  const int canonical_code_length = iree_snprintf(
      canonical_code, sizeof(canonical_code), "%03u", (unsigned)code);
  if (canonical_code_length <= 0 ||
      !iree_string_view_equal(
          code_text,
          iree_make_string_view(canonical_code, canonical_code_length))) {
    return false;
  }
  *out_ref = LOOM_ERROR_REF(domain, code);
  return true;
}

const char* loom_emitter_name(loom_emitter_t emitter) {
  if (emitter < IREE_ARRAYSIZE(loom_emitter_names)) {
    const char* name = loom_emitter_names[emitter];
    if (name != NULL) {
      return name;
    }
  }
  return "unknown";
}

const loom_error_def_t* loom_error_catalog_lookup(
    const loom_error_catalog_t* catalog, loom_error_domain_t domain,
    uint16_t code) {
  if (domain >= LOOM_ERROR_DOMAIN_COUNT_) {
    return NULL;
  }
  for (const loom_error_catalog_t* current_catalog = catalog;
       current_catalog != NULL;
       current_catalog = current_catalog->fallback_catalog) {
    const loom_error_domain_span_t domain_span =
        current_catalog->domain_spans[domain];
    if (code >= domain_span.code_count) {
      continue;
    }
    const uint16_t code_index = domain_span.code_index_start + code;
    const uint16_t error_index =
        current_catalog->error_indices_by_code[code_index];
    if (error_index != UINT16_MAX) {
      return &current_catalog->error_defs[error_index];
    }
  }
  return NULL;
}

const loom_error_def_t* loom_error_catalog_lookup_ref(
    const loom_error_catalog_t* catalog, loom_error_ref_t ref) {
  if (!loom_error_ref_is_set(ref)) {
    return NULL;
  }
  return loom_error_catalog_lookup(catalog, loom_error_ref_domain(ref),
                                   loom_error_ref_code(ref));
}

const loom_error_def_t* loom_error_def_lookup(loom_error_domain_t domain,
                                              uint16_t code) {
  return loom_error_catalog_lookup(&loom_error_catalog_core, domain, code);
}

const loom_error_def_t* loom_error_def_lookup_ref(loom_error_ref_t ref) {
  return loom_error_catalog_lookup_ref(&loom_error_catalog_core, ref);
}
