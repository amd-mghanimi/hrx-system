// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>

#include "libamdf/cts/gpu/kernels/lds_exchange.h"
#include "libamdf/cts/gpu/kernels/lds_exchange_wave64_kernels.h"
#include "libamdf/cts/gpu/kernels/product_test.h"

namespace {

TEST(KernelTest, EveryAlternateLdsProductPreservesTheWave64CallerContract) {
  using Arguments = kernels::lds_exchange::Arguments;
  ASSERT_FALSE(kernels::lds_exchange_wave64::kKernels.variants.empty());
  for (const auto& kernel : kernels::lds_exchange_wave64::kKernels.variants) {
    SCOPED_TRACE(kernel.target);
    kernels::testing::CheckArgumentLayout(
        kernel, kernels::lds_exchange::kArgumentByteOffsets,
        kernels::lds_exchange::kArgumentByteLengths,
        kernels::lds_exchange::kArgumentValueKinds, sizeof(Arguments),
        alignof(Arguments));
    EXPECT_EQ(kernel.arguments.byte_length, sizeof(Arguments));
    EXPECT_EQ(kernel.required_workgroup_size,
              (std::array<uint32_t, 3>{128, 1, 1}));
    EXPECT_EQ(kernel.group_segment_byte_length, 512u);
    EXPECT_EQ(kernel.private_segment_byte_length, 0u);
    EXPECT_EQ(kernel.wavefront_size, 64u);
    EXPECT_EQ(kernel.program.code_properties, 8u);
    EXPECT_EQ(kernel.program.argument_preload, 0u);
    EXPECT_EQ(kernel.program.resource2 & 0x1fffu, 0x84u);
  }
}

}  // namespace
