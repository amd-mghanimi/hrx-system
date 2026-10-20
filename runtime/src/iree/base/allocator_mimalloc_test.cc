// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <mimalloc.h>

#include <cstdint>
#include <limits>
#include <new>

#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class ScopedNewHandler {
 public:
  explicit ScopedNewHandler(std::new_handler handler)
      : previous_handler_(std::set_new_handler(handler)) {}
  ~ScopedNewHandler() { std::set_new_handler(previous_handler_); }

 private:
  // Process handler restored when the test scope exits.
  std::new_handler previous_handler_;
};

struct NewHandlerInvoked {};

void ThrowFromNewHandler() { throw NewHandlerInvoked{}; }

void AllocateImpossibleSize() {
  void* allocation = ::operator new(std::numeric_limits<std::size_t>::max());
  ::operator delete(allocation);
}

void ExpectMimallocAllocation(void* allocation) {
  ASSERT_NE(allocation, nullptr);
  EXPECT_TRUE(mi_is_in_heap_region(allocation));
}

TEST(MimallocAllocatorTest, OwnsSystemAllocations) {
  iree_allocator_t allocator = iree_allocator_system();
  void* allocation = nullptr;
  IREE_ASSERT_OK(
      iree_allocator_malloc_uninitialized(allocator, 64, &allocation));
  ASSERT_NE(allocation, nullptr);
  EXPECT_TRUE(mi_is_in_heap_region(allocation));

  IREE_ASSERT_OK(iree_allocator_realloc(allocator, 256, &allocation));
  ASSERT_NE(allocation, nullptr);
  EXPECT_TRUE(mi_is_in_heap_region(allocation));

  iree_allocator_free(allocator, allocation);
}

TEST(MimallocAllocatorTest, ZeroInitializesSystemAllocations) {
  iree_allocator_t allocator = iree_allocator_system();
  void* allocation = nullptr;
  IREE_ASSERT_OK(iree_allocator_malloc(allocator, 64, &allocation));
  ASSERT_NE(allocation, nullptr);
  const auto* bytes = static_cast<const std::uint8_t*>(allocation);
  for (iree_host_size_t i = 0; i < 64; ++i) {
    EXPECT_EQ(bytes[i], 0);
  }
  iree_allocator_free(allocator, allocation);
}

TEST(MimallocAllocatorTest, OwnsCompleteGlobalCxxAllocationFamily) {
  void* scalar_unsized = ::operator new(64);
  ExpectMimallocAllocation(scalar_unsized);
  ::operator delete(scalar_unsized);

  void* array_unsized = ::operator new[](64);
  ExpectMimallocAllocation(array_unsized);
  ::operator delete[](array_unsized);

  void* scalar_sized = ::operator new(64);
  ExpectMimallocAllocation(scalar_sized);
  ::operator delete(scalar_sized, 64);

  void* array_sized = ::operator new[](64);
  ExpectMimallocAllocation(array_sized);
  ::operator delete[](array_sized, 64);

  void* scalar_nothrow = ::operator new(64, std::nothrow);
  ExpectMimallocAllocation(scalar_nothrow);
  ::operator delete(scalar_nothrow, std::nothrow);

  void* array_nothrow = ::operator new[](64, std::nothrow);
  ExpectMimallocAllocation(array_nothrow);
  ::operator delete[](array_nothrow, std::nothrow);

#if defined(__cpp_aligned_new)
  constexpr std::size_t kAlignment = 64;
  auto alignment = static_cast<std::align_val_t>(kAlignment);
  auto expect_aligned_allocation = [=](void* allocation) {
    ExpectMimallocAllocation(allocation);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(allocation) % kAlignment, 0u);
  };

  void* scalar_aligned = ::operator new(64, alignment);
  expect_aligned_allocation(scalar_aligned);
  ::operator delete(scalar_aligned, alignment);

  void* array_aligned = ::operator new[](64, alignment);
  expect_aligned_allocation(array_aligned);
  ::operator delete[](array_aligned, alignment);

  void* scalar_sized_aligned = ::operator new(64, alignment);
  expect_aligned_allocation(scalar_sized_aligned);
  ::operator delete(scalar_sized_aligned, 64, alignment);

  void* array_sized_aligned = ::operator new[](64, alignment);
  expect_aligned_allocation(array_sized_aligned);
  ::operator delete[](array_sized_aligned, 64, alignment);

  void* scalar_nothrow_aligned = ::operator new(64, alignment, std::nothrow);
  expect_aligned_allocation(scalar_nothrow_aligned);
  ::operator delete(scalar_nothrow_aligned, alignment, std::nothrow);

  void* array_nothrow_aligned = ::operator new[](64, alignment, std::nothrow);
  expect_aligned_allocation(array_nothrow_aligned);
  ::operator delete[](array_nothrow_aligned, alignment, std::nothrow);
#endif  // defined(__cpp_aligned_new)

  void* zero_size = ::operator new(0);
  ExpectMimallocAllocation(zero_size);
  ::operator delete(zero_size);
}

TEST(MimallocAllocatorTest, HonorsNewHandler) {
  ScopedNewHandler handler(ThrowFromNewHandler);
  EXPECT_THROW(AllocateImpossibleSize(), NewHandlerInvoked);
  EXPECT_EQ(
      ::operator new(std::numeric_limits<std::size_t>::max(), std::nothrow),
      nullptr);
}

TEST(MimallocAllocatorTest, ReportsAllocationFailure) {
  ScopedNewHandler handler(nullptr);
  EXPECT_THROW(AllocateImpossibleSize(), std::bad_alloc);
  EXPECT_EQ(
      ::operator new(std::numeric_limits<std::size_t>::max(), std::nothrow),
      nullptr);
}

}  // namespace
