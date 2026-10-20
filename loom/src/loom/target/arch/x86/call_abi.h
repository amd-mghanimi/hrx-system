// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// x86-64 SysV source-value classification shared by lowering and emission.

#ifndef LOOM_TARGET_ARCH_X86_CALL_ABI_H_
#define LOOM_TARGET_ARCH_X86_CALL_ABI_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_x86_call_abi_class_e {
  LOOM_X86_CALL_ABI_CLASS_INTEGER = 0,
  LOOM_X86_CALL_ABI_CLASS_SSE = 1,
} loom_x86_call_abi_class_t;

// Inbound action required after transporting one platform value into its Low
// carrier. Outbound transport only exposes the significant low bits.
typedef enum loom_x86_call_abi_value_action_e {
  LOOM_X86_CALL_ABI_VALUE_ACTION_NONE = 0,
  LOOM_X86_CALL_ABI_VALUE_ACTION_NORMALIZE_I1 = 1,
  LOOM_X86_CALL_ABI_VALUE_ACTION_NORMALIZE_I8 = 2,
  LOOM_X86_CALL_ABI_VALUE_ACTION_NORMALIZE_I16 = 3,
} loom_x86_call_abi_value_action_t;

// Physical classes and stack properties for one source-typed ABI value.
typedef struct loom_x86_call_abi_classification_t {
  // Independent SysV register bank consumed by the value.
  loom_x86_call_abi_class_t abi_class;
  // Low register class used inside the function body.
  uint16_t carrier_register_class;
  // Platform register class used at the function boundary.
  uint16_t boundary_register_class;
  // Significant byte count in a register or stack slot.
  uint16_t byte_length;
  // Natural stack alignment in bytes.
  uint8_t byte_alignment;
  // loom_x86_call_abi_value_action_t required after inbound transport.
  uint8_t action;
} loom_x86_call_abi_classification_t;

// Classifies one source type admitted by the native x86-64 SysV boundary.
// Source views are transported as their projected data address. Compiler-only
// values and vectors without an exact architectural register width fail.
bool loom_x86_call_abi_classify_source_type(
    loom_type_t source_type,
    loom_x86_call_abi_classification_t* out_classification);

// Classifies one source type using an exact Low carrier register class. Most
// source types have one carrier. Narrow floating values additionally admit XMM
// so feature profiles with native scalar arithmetic can retain the platform
// representation inside the function body.
bool loom_x86_call_abi_classify_source_carrier(
    loom_type_t source_type, uint16_t carrier_register_class,
    loom_x86_call_abi_classification_t* out_classification);

// Returns the byte width of a general-purpose or SIMD call register class.
uint16_t loom_x86_call_abi_register_byte_length(uint16_t register_class);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_X86_CALL_ABI_H_
