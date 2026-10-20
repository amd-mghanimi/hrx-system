// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0

#include "hrx_internal.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(StatusTest, PreservesEverySharedCodeAcrossTheHalBoundary) {
  for (int code = IREE_STATUS_OK; code <= IREE_STATUS_DATA_LOSS; ++code) {
    hrx_status_t status = hrx_status_from_iree(
        iree_status_from_code(static_cast<iree_status_code_t>(code)));
    EXPECT_EQ(static_cast<hrx_status_code_t>(code), hrx_status_code(status));
    iree_status_t roundtrip = hrx_status_to_iree(status);
    EXPECT_EQ(static_cast<iree_status_code_t>(code),
              iree_status_code(roundtrip));
    iree_status_free(roundtrip);
  }
}

TEST(StatusTest, UnrepresentedHalCodeRemainsUnknown) {
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNKNOWN,
      hrx_status_to_iree(hrx_status_from_iree(
          iree_status_from_code(IREE_STATUS_UNAUTHENTICATED))));
}

}  // namespace
