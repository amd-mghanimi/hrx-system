// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/target_requirements.h"

#include <cxx/ast.h>
#include <cxx/decl.h>
#include <cxx/names.h>
#include <cxx/symbols.h>

#include <string_view>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* function_where(Source& source, std::string_view name) {
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    auto* simple = cxx::ast_cast<cxx::SimpleDeclarationAST>(declaration);
    if (!simple) {
      continue;
    }
    for (auto* declarator : cxx::ListView{simple->initDeclaratorList}) {
      auto* function =
          cxx::symbol_cast<cxx::FunctionSymbol>(declarator->symbol);
      if (!function || !function->name() ||
          cxx::to_string(function->name()) != name) {
        continue;
      }
      auto* prototype = cxx::getFunctionPrototype(declarator->declarator);
      cxx::ExpressionAST* expression = nullptr;
      visit_loom_attributes(
          source.unit(), prototype ? prototype->attributeList : nullptr,
          [&](std::string_view spelling, cxx::AttributeAST* attribute) {
            if (spelling != "where") {
              return;
            }
            auto* clause = attribute->attributeArgumentClause;
            auto* arguments = clause ? clause->expressionList : nullptr;
            expression =
                arguments && !arguments->next ? arguments->value : nullptr;
          });
      return expression;
    }
  }
  return nullptr;
}

loom_cxx_import_options_t options() {
  loom_cxx_import_options_t result;
  loom_cxx_import_options_initialize(&result);
  return result;
}

TEST(TargetRequirementsTest, ProjectsSemanticSubgroupSizeEquality) {
  auto import_options = options();
  Source source(IREE_SV(R"cpp(
#include <loomcxx/target.h>
                  unsigned wave64(unsigned value)
                      [[loom::where(loom::target::subgroup::size() == 64u)]];
                  unsigned wave32(unsigned value)
                      [[loom::where(32u == loom::target::subgroup::size())]];
                )cpp"),
                IREE_SV("requirements.cxx"), import_options);

  auto* wave64_expression = function_where(source, "wave64");
  ASSERT_NE(wave64_expression, nullptr);
  auto wave64 = project_target_requirement(source.unit(), source.diagnostics(),
                                           wave64_expression);
  ASSERT_TRUE(wave64.has_value());
  EXPECT_EQ(wave64->kind, TargetRequirementKind::SubgroupSize);
  EXPECT_EQ(wave64->value, 64);

  auto* wave32_expression = function_where(source, "wave32");
  ASSERT_NE(wave32_expression, nullptr);
  auto wave32 = project_target_requirement(source.unit(), source.diagnostics(),
                                           wave32_expression);
  ASSERT_TRUE(wave32.has_value());
  EXPECT_EQ(wave32->kind, TargetRequirementKind::SubgroupSize);
  EXPECT_EQ(wave32->value, 32);
}

TEST(TargetRequirementsTest, LeavesValuePredicatesToTheirOwner) {
  auto import_options = options();
  Source source(IREE_SV(R"cpp(
                  unsigned positive(unsigned value) [[loom::where(value > 0u)]];
                )cpp"),
                IREE_SV("value.cxx"), import_options);
  auto* expression = function_where(source, "positive");
  ASSERT_NE(expression, nullptr);
  EXPECT_FALSE(project_target_requirement(source.unit(), source.diagnostics(),
                                          expression));
}

TEST(TargetRequirementsTest, RejectsInexactTargetComparisons) {
  auto import_options = options();
  Source source(IREE_SV(R"cpp(
#include <loomcxx/target.h>
                  unsigned not_wave64(unsigned value)
                      [[loom::where(loom::target::subgroup::size() != 64u)]];
                )cpp"),
                IREE_SV("inexact.cxx"), import_options);
  auto* expression = function_where(source, "not_wave64");
  ASSERT_NE(expression, nullptr);
  EXPECT_THROW(project_target_requirement(source.unit(), source.diagnostics(),
                                          expression),
               SourceRejected);
}

}  // namespace
}  // namespace loom::cxx_import
