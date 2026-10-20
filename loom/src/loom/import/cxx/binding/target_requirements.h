// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_TARGET_REQUIREMENTS_H_
#define LOOM_IMPORT_CXX_BINDING_TARGET_REQUIREMENTS_H_

#include <cxx/ast_fwd.h>

#include <cstdint>
#include <optional>

#include "loom/ir/attribute.h"

namespace cxx {
class TranslationUnit;
}

namespace loom::cxx_import {

class Diagnostics;

// Target-condition families expressible by the public C++ query surface.
enum class TargetRequirementKind {
  SubgroupSize,
};

// One target applicability requirement projected from a pure source
// comparison. The source is retained only for admission diagnostics.
struct ProjectedTargetRequirement {
  // Existing parameterized target-condition family selected by the query.
  TargetRequirementKind kind;
  // Exact integer parameter carried by the selected family.
  int64_t value;
  // Complete source comparison owning diagnostics.
  cxx::ExpressionAST* source;
};

// Projects one source expression when it directly compares a semantically
// bound target query to a pure constant. Returns no value when the expression
// contains no recognized target query; recognized but unrepresentable target
// comparisons diagnose instead of falling through to value predicates.
std::optional<ProjectedTargetRequirement> project_target_requirement(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    cxx::ExpressionAST* expression);

// Materializes one admitted requirement in module-owned immutable storage.
loom_attribute_t materialize_target_requirement(
    loom_module_t* module, const ProjectedTargetRequirement& requirement);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_TARGET_REQUIREMENTS_H_
