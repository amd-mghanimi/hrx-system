// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <mimalloc.h>

#include <cstddef>
#include <new>

namespace {

using AllocationFunction = void*(std::size_t size, std::size_t alignment);

// The mimalloc core is deliberately compiled as C so pure C products retain a
// C-only closure. Its mi_new family therefore cannot throw std::bad_alloc.
// Implement the C++ failure contract here using the ordinary allocation API.
void* Allocate(std::size_t size, std::size_t alignment) {
  if (size == 0) {
    size = 1;
  }
  for (;;) {
    void* allocation =
        alignment == 0 ? mi_malloc(size) : mi_malloc_aligned(size, alignment);
    if (allocation) {
      return allocation;
    }

    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      throw std::bad_alloc();
    }
    handler();
  }
}

void* AllocateUnaligned(std::size_t size, std::size_t alignment) {
  (void)alignment;
  return Allocate(size, 0);
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
  return Allocate(size, alignment);
}

void* AllocateNothrow(AllocationFunction allocation_function, std::size_t size,
                      std::size_t alignment) noexcept {
  try {
    return allocation_function(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

}  // namespace

void* operator new(std::size_t size) { return AllocateUnaligned(size, 0); }

void* operator new[](std::size_t size) { return AllocateUnaligned(size, 0); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateNothrow(AllocateUnaligned, size, 0);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateNothrow(AllocateUnaligned, size, 0);
}

void operator delete(void* pointer) noexcept { mi_free(pointer); }

void operator delete[](void* pointer) noexcept { mi_free(pointer); }

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  mi_free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  mi_free(pointer);
}

void operator delete(void* pointer, std::size_t size) noexcept {
  mi_free_size(pointer, size);
}

void operator delete[](void* pointer, std::size_t size) noexcept {
  mi_free_size(pointer, size);
}

#if defined(__cpp_aligned_new)

void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return AllocateNothrow(AllocateAligned, size,
                         static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return AllocateNothrow(AllocateAligned, size,
                         static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer, std::align_val_t alignment) noexcept {
  mi_free_aligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
  mi_free_aligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  mi_free_aligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete[](void* pointer, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
  mi_free_aligned(pointer, static_cast<std::size_t>(alignment));
}

void operator delete(void* pointer, std::size_t size,
                     std::align_val_t alignment) noexcept {
  mi_free_size_aligned(pointer, size, static_cast<std::size_t>(alignment));
}

void operator delete[](void* pointer, std::size_t size,
                       std::align_val_t alignment) noexcept {
  mi_free_size_aligned(pointer, size, static_cast<std::size_t>(alignment));
}

#endif  // defined(__cpp_aligned_new)
