// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_
#define AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_

#include "libamdf/cts/gpu/kernels/kernel.h"
#include "libamdf/cts/gpu/linux/io_uring/file_io_resources.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

// Runs finite PM4 file owners with the shared native I/O lifetime and service.
class GpuFileIoFixture : public Pm4DispatchTest, protected GpuFileIoResources {
 protected:
  void SetUp() override;
  void TearDown() override;

  // Runs one finite owner with the selected control service, then retires it.
  void Execute(const kernels::Kernel& kernel, GpuMemory* arguments,
               GpuMemory* completion, const char* property_prefix);
};

#endif  // AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_
