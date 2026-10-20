// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_COMBINING_H_
#define LOOM_IMPORT_CXX_BINDING_COMBINING_H_

#include <optional>
#include <string_view>

#include "loom/ops/combining.h"

namespace loom::cxx_import {

// Resolves the canonical High combining-kind spelling shared by vector and
// kernel collective bindings. Unknown spellings remain available to another
// operation binding rather than diagnosing at this vocabulary boundary.
std::optional<loom_combining_kind_t> parse_combining_kind(
    std::string_view name);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_COMBINING_H_
