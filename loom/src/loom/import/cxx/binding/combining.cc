// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/combining.h"

#include <utility>

namespace loom::cxx_import {

std::optional<loom_combining_kind_t> parse_combining_kind(
    std::string_view name) {
  static constexpr std::pair<std::string_view, loom_combining_kind_t> kinds[] =
      {
          {"addi", LOOM_COMBINING_KIND_ADDI},
          {"addf", LOOM_COMBINING_KIND_ADDF},
          {"muli", LOOM_COMBINING_KIND_MULI},
          {"mulf", LOOM_COMBINING_KIND_MULF},
          {"minsi", LOOM_COMBINING_KIND_MINSI},
          {"maxsi", LOOM_COMBINING_KIND_MAXSI},
          {"minui", LOOM_COMBINING_KIND_MINUI},
          {"maxui", LOOM_COMBINING_KIND_MAXUI},
          {"andi", LOOM_COMBINING_KIND_ANDI},
          {"ori", LOOM_COMBINING_KIND_ORI},
          {"xori", LOOM_COMBINING_KIND_XORI},
          {"minimumf", LOOM_COMBINING_KIND_MINIMUMF},
          {"maximumf", LOOM_COMBINING_KIND_MAXIMUMF},
          {"minnumf", LOOM_COMBINING_KIND_MINNUMF},
          {"maxnumf", LOOM_COMBINING_KIND_MAXNUMF},
      };
  for (auto [candidate, kind] : kinds) {
    if (candidate == name) {
      return kind;
    }
  }
  return std::nullopt;
}

}  // namespace loom::cxx_import
