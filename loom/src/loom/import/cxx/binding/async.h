// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_ASYNC_H_
#define LOOM_IMPORT_CXX_BINDING_ASYNC_H_

#include <cxx/attributes.h>
#include <cxx/symbols_fwd.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "loom/import/cxx/value/representation.h"
#include "loom/import/cxx/value/types.h"
#include "loom/ops/cache.h"
#include "loom/ops/kernel/ops.h"

namespace loom::cxx_import {

// A source projection of semantic asynchronous data movement. Concrete
// specializations retain explicit direction and cache policy while High owns
// transfer legality, token/group lifetime, and target realization.
class AsyncIntrinsic {
 public:
  static bool supports(std::string_view name);

  // Unknown operations return nullopt. Invalid selectors, signatures, and
  // endpoint qualifiers diagnose at owner and throw SourceRejected.
  static std::optional<AsyncIntrinsic> resolve(cxx::TranslationUnit& unit,
                                               Diagnostics& diagnostics,
                                               Types& types,
                                               cxx::FunctionSymbol* function,
                                               const cxx::Attribute& attribute,
                                               cxx::AST* owner);

  // Emits one admitted transfer, group, or wait from already evaluated source
  // values. Transfer and group results retain their opaque dialect partition.
  std::optional<Value> call(std::span<const Value> arguments, ValueArena& arena,
                            loom_builder_t* builder,
                            loom_location_id_t location) const;

  bool equivalent(const AsyncIntrinsic& other) const;

 private:
  enum class Operation {
    Copy,
    CopyMask,
    Gather,
    GatherMask,
    ClusterGather,
    ClusterGatherMask,
    Group,
    Wait,
  };

  AsyncIntrinsic(Operation operation, loom_cache_scope_t cache_scope,
                 loom_cache_temporal_t cache_temporal,
                 loom_kernel_direction_t direction, uint16_t newer_groups,
                 const OpaqueDialectPartition* result)
      : operation_(operation),
        cache_scope_(cache_scope),
        cache_temporal_(cache_temporal),
        direction_(direction),
        newer_groups_(newer_groups),
        result_(result) {}

  static std::optional<Operation> find(std::string_view name);

  // Semantic operation selected at concrete specialization admission.
  Operation operation_;
  // Advisory logical cache-policy scope; unused by group and wait.
  loom_cache_scope_t cache_scope_;
  // Advisory temporal policy; unused by group and wait.
  loom_cache_temporal_t cache_temporal_;
  // Copy direction; unused by gather, group, and wait.
  loom_kernel_direction_t direction_;
  // Younger groups permitted to remain outstanding; used only by wait.
  uint16_t newer_groups_;
  // Exact opaque token/group result partition; null for wait.
  const OpaqueDialectPartition* result_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_ASYNC_H_
