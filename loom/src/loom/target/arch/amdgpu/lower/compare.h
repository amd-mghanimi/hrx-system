// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Private AMDGPU comparison and floating-point clamp lowering contracts.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_COMPARE_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_COMPARE_H_

#include <stdint.h>

#include "loom/codegen/low/lower/lower.h"
#include "loom/ir/ir.h"
#include "loom/target/arch/amdgpu/lower/mask.h"
#include "loom/target/low_legality.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sparse selected inline lanes in increasing logical lane order.
typedef struct loom_amdgpu_compare_lane_plan_t loom_amdgpu_compare_lane_plan_t;

typedef struct loom_amdgpu_vector_compare_plan_t {
  // Left-hand payload vector value.
  loom_value_id_t lhs;
  // Right-hand payload vector value.
  loom_value_id_t rhs;
  // Descriptor row selected for the compare predicate.
  loom_low_lower_resolved_descriptor_t descriptor;
  // Optional descriptor row selected when the left-hand lane is inline.
  loom_low_lower_resolved_descriptor_t src0_inline_descriptor;
  // Optional descriptor row selected when the right-hand lane is inline.
  loom_low_lower_resolved_descriptor_t src1_inline_descriptor;
  // Function-plan-owned selected inline lanes, or NULL for register-only input.
  const loom_amdgpu_compare_lane_plan_t* inline_lanes;
  // Result mask vector value.
  loom_value_id_t result;
  // Static number of payload and mask lanes compared.
  uint8_t lane_count;
  // Number of selected entries in inline_lanes.
  uint8_t inline_lane_count;
} loom_amdgpu_vector_compare_plan_t;

typedef enum loom_amdgpu_float_classification_form_e {
  LOOM_AMDGPU_FLOAT_CLASSIFICATION_FORM_NONE = 0,
  LOOM_AMDGPU_FLOAT_CLASSIFICATION_FORM_INLINE = 1,
  LOOM_AMDGPU_FLOAT_CLASSIFICATION_FORM_LITERAL = 2,
  LOOM_AMDGPU_FLOAT_CLASSIFICATION_FORM_REGISTER = 3,
} loom_amdgpu_float_classification_form_t;

typedef struct loom_amdgpu_vector_float_classification_plan_t {
  // Floating-point payload vector being classified.
  loom_value_id_t input;
  // Descriptor selected for low halves or whole-width lanes.
  loom_low_lower_resolved_descriptor_t low_descriptor;
  // Descriptor selected for packed F16 high halves.
  loom_low_lower_resolved_descriptor_t high_descriptor;
  // Result mask vector receiving one native lane mask per input lane.
  loom_value_id_t result;
  // Exact ten-bit hardware class mask for the source operation.
  uint32_t class_mask;
  // Static number of logical input and result lanes.
  uint32_t lane_count;
  // Floating-point type carried by each logical input lane.
  loom_scalar_type_t element_type;
  // Selected class-mask operand representation.
  loom_amdgpu_float_classification_form_t form;
} loom_amdgpu_vector_float_classification_plan_t;

typedef enum loom_amdgpu_clampf_mode_e {
  LOOM_AMDGPU_CLAMPF_MODE_NONE = 0,
  LOOM_AMDGPU_CLAMPF_MODE_ORDERED = 1,
  LOOM_AMDGPU_CLAMPF_MODE_NUMBER = 2,
} loom_amdgpu_clampf_mode_t;

// Selected immediate bounds in logical lane order, lower before upper.
typedef struct loom_amdgpu_clampf_literal_t loom_amdgpu_clampf_literal_t;

typedef struct loom_amdgpu_clampf_plan_t {
  // Source payload being clamped.
  loom_value_id_t value;
  // Source lower bound.
  loom_value_id_t lower;
  // Source upper bound.
  loom_value_id_t upper;
  // Selected clamp semantics with native AMDGPU packet support.
  loom_amdgpu_clampf_mode_t mode;
  // Descriptor row selected for the ordered lower-bound comparison.
  loom_low_lower_resolved_descriptor_t lower_compare_descriptor;
  // Descriptor row selected for the ordered upper-bound comparison.
  loom_low_lower_resolved_descriptor_t upper_compare_descriptor;
  // Descriptor rows selected for ordered-mode v_cndmask_b32 lane selects.
  loom_amdgpu_cndmask_b32_descriptors_t select_descriptors;
  // Descriptor row selected for register-register lower-bound maxnum.
  loom_low_lower_resolved_descriptor_t lower_bound_register_descriptor;
  // Optional descriptor row selected for literal lower-bound maxnum.
  loom_low_lower_resolved_descriptor_t lower_bound_literal_descriptor;
  // Descriptor row selected for register-register upper-bound minnum.
  loom_low_lower_resolved_descriptor_t upper_bound_register_descriptor;
  // Optional descriptor row selected for literal upper-bound minnum.
  loom_low_lower_resolved_descriptor_t upper_bound_literal_descriptor;
  // Function-plan-owned selected bounds, or NULL for register-only input.
  const loom_amdgpu_clampf_literal_t* literals;
  // Result value.
  loom_value_id_t result;
  // Static number of f32 lanes lowered.
  uint8_t lane_count;
  // Number of selected immediate bounds in literals.
  uint8_t literal_count;
} loom_amdgpu_clampf_plan_t;

// Selects the AMDGPU mask compare plan for a source vector.cmpi op.
iree_status_t loom_amdgpu_select_vector_cmpi_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_vector_compare_plan_t* out_plan, bool* out_selected);

// Lowers a source vector.cmpi op from its selected AMDGPU mask compare plan.
iree_status_t loom_amdgpu_lower_vector_cmpi(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_vector_compare_plan_t* plan);

// Selects the AMDGPU mask compare plan for a source vector.cmpf op.
iree_status_t loom_amdgpu_select_vector_cmpf_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_vector_compare_plan_t* out_plan, bool* out_selected);

// Lowers a source vector.cmpf op from its selected AMDGPU mask compare plan.
iree_status_t loom_amdgpu_lower_vector_cmpf(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_vector_compare_plan_t* plan);

// Selects a native AMDGPU vector floating-point classification plan.
iree_status_t loom_amdgpu_select_vector_float_classification_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_vector_float_classification_plan_t* out_plan,
    bool* out_selected);

// Lowers a vector floating-point classification op from its selected plan.
iree_status_t loom_amdgpu_lower_vector_float_classification(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_vector_float_classification_plan_t* plan);

// Selects the AMDGPU clamp plan for a source scalar.clampf op.
iree_status_t loom_amdgpu_select_scalar_clampf_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_clampf_plan_t* out_plan, bool* out_selected);

// Selects the AMDGPU clamp plan for a source vector.clampf op.
iree_status_t loom_amdgpu_select_vector_clampf_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_clampf_plan_t* out_plan, bool* out_selected);

// Verifies AMDGPU low legality for callback-lowered clamp recipes.
iree_status_t loom_amdgpu_low_legality_verify_clampf(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled);

// Lowers a source scalar.clampf or vector.clampf op from its selected AMDGPU
// plan.
iree_status_t loom_amdgpu_lower_clampf(loom_low_lower_context_t* context,
                                       const loom_op_t* source_op,
                                       const loom_amdgpu_clampf_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_COMPARE_H_
