// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>

unsigned comparison_chain(unsigned first, unsigned second, unsigned third,
                          unsigned fourth, unsigned fifth, unsigned sixth,
                          unsigned seventh) {
  return first < 256u && second < 256u && third < 256u && fourth < 256u &&
         fifth < 256u && sixth < 256u && seventh < 256u;
}

unsigned comparison_input(loom::check::ordinal trial, unsigned position) {
  unsigned trial_value = trial;
  if (trial_value < 128u) {
    return trial_value & (1u << position) ? 256u : 255u;
  }
  return trial_value == 129u + position ? ~0u : 0u;
}

unsigned comparison_oracle(unsigned first, unsigned second, unsigned third,
                           unsigned fourth, unsigned fifth, unsigned sixth,
                           unsigned seventh) {
  return (first < 256u) & (second < 256u) & (third < 256u) & (fourth < 256u) &
         (fifth < 256u) & (sixth < 256u) & (seventh < 256u);
}

LOOM_CHECK_SCENARIO(comparison_functions) {
  loom::check::trial<136>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto first = loom::check::generate<comparison_input>(trial, 0u);
        const auto second = loom::check::generate<comparison_input>(trial, 1u);
        const auto third = loom::check::generate<comparison_input>(trial, 2u);
        const auto fourth = loom::check::generate<comparison_input>(trial, 3u);
        const auto fifth = loom::check::generate<comparison_input>(trial, 4u);
        const auto sixth = loom::check::generate<comparison_input>(trial, 5u);
        const auto seventh = loom::check::generate<comparison_input>(trial, 6u);
        loom::check::compare<comparison_chain, comparison_oracle>(
            first, second, third, fourth, fifth, sixth, seventh,
            [](unsigned actual, unsigned expected) {
              loom::check::expect_equal(actual, expected);
            });
      });
}
