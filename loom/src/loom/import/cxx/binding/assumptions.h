// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_
#define LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_

#include <vector>

#include "loom/import/cxx/binding/predicates.h"

namespace loom::cxx_import {

// Admits an assume-annotated call as a conjunction of exactly representable
// predicates, in source order. This is the call-shape adapter over the shared
// semantic projector; body translation owns the local-binding restrictions and
// conversion materialization needed to publish refinements.
std::vector<ProjectedPredicate> assumption_predicates(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    cxx::CallExpressionAST* call);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_
