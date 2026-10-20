// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_ENCODING_H_
#define LOOMCXX_ENCODING_H_

#include <loomcxx/encoding_type.h>

namespace loom::encoding {

// Numeric formats for encoded operands and their scale values. Enum names
// match the encoding dialect; their C++ values are independent of compiler
// ordinals.
enum class numeric_format {
  none,
  f64,
  f32,
  tf32,
  f16,
  bf16,
  i32,
  u32,
  i16,
  u16,
  i8,
  u8,
  i6,
  u6,
  i5,
  u5,
  i4,
  u4,
  i3,
  u3,
  i2,
  u2,
  i1,
  u1,
  f8e4m3,
  f8e5m2,
  f8e4m3fn,
  f8e4m3fnuz,
  f8e5m2fnuz,
  e8m0,
  bf8,
  f6e3m2,
  f6e2m3,
  bf6,
  f4e2m1,
  ternary,
  sign_bit,
  codebook_index,
  quant_i8,
  quant_i6,
  quant_i4,
  bfp16ebs8,
};

// Physical ordering of encoded payload elements.
enum class payload_packing {
  dense_lanes,
  little_endian_nibbles,
  big_endian_nibbles,
  bitfield_stream,
  bitplane_stream,
  multi_stream,
  base_n_packed,
  codebook_indices,
  target_fragment,
  interleaved_scale_payload,
  separate_scale_payload,
};

// Logical groups sharing an auxiliary scale.
enum class scale_topology {
  none,
  tensor_global,
  row,
  column,
  channel,
  group_1d,
  block_1d,
  block_2d,
  subblock_in_superblock,
  hierarchical,
  per_token,
  per_head,
  per_page,
  runtime_amax_derived,
};

// Affine interpretation applied after numeric decoding.
enum class affine_policy {
  none,
  scale_only,
  scale_plus_min,
  scale_plus_zero_point,
  scale_plus_bias,
  super_scale_times_subscale,
  sum_correction,
};

// E2M1 schema parameters, defaulting to one MXFP4 group of 32 values. Packed
// word counts derive from the payload size unless overridden. Runtime scales
// belong to vector.decode's auxiliary record.
struct f4e2m1 {
  // Logical value count decoded from the physical payload.
  unsigned payload_elements = 32;
  // Number of packed 32-bit words, rounded up to cover the logical payload.
  unsigned payload_registers = (payload_elements + 7ull) / 8;
  // Nibble ordering within each payload word.
  encoding::payload_packing payload_packing =
      encoding::payload_packing::little_endian_nibbles;
  // Numeric format of the explicit scale values.
  numeric_format scale_format = numeric_format::e8m0;
  // Number of consecutive values sharing a scale.
  unsigned scale_group_elements = 32;
  // Number of named scale operands, independently of each operand's lanes.
  unsigned scale_operands = 1;
  // Logical organization of the scale groups.
  encoding::scale_topology scale_topology = encoding::scale_topology::block_1d;
  // Apply the block scale to decoded numeric values.
  affine_policy affine = affine_policy::scale_only;
  // Preserve the E8M0 minimum-scale interpretation when its code is zero.
  bool zero_scale_fallback = true;
};

// Finite E4M3 schema parameters, defaulting to one MXFP8 group of 32 values.
// Payload lanes have the ordinary float8_e4m3fn_t vector representation.
struct f8e4m3fn {
  // Logical value count, equal to the FP8 payload's lane count.
  unsigned payload_elements = 32;
  // Numeric format of the explicit scale values.
  numeric_format scale_format = numeric_format::e8m0;
  // Number of consecutive values sharing a scale.
  unsigned scale_group_elements = 32;
  // Number of named scale operands, independently of each operand's lanes.
  unsigned scale_operands = 1;
  // Logical organization of the scale groups.
  encoding::scale_topology scale_topology = encoding::scale_topology::block_1d;
  // Apply the block scale to decoded numeric values.
  affine_policy affine = affine_policy::scale_only;
};

// Defines a first-class schema from static parameters. The aggregate is a C++
// constant template argument; the returned value can pass through ordinary
// helpers and records. Custom families use the same loom::op binding with a
// parameter aggregate whose fields match the family's public schema.
template <f4e2m1 Parameters>
[[loom::op("encoding.define", "encoding.f4e2m1")]]
type::encoding<role::schema> define();

template <f8e4m3fn Parameters>
[[loom::op("encoding.define", "encoding.f8e4m3fn")]]
type::encoding<role::schema> define();

// Composes an address layout and storage schema for the same logical rank.
// Dynamic schema auxiliaries remain explicit operands of decode operations.
template <type::size_type Rank>
[[loom::op("encoding.define")]]
type::encoding<role::storage, Rank> define(
    type::encoding<role::layout, Rank> layout,
    type::encoding<role::schema> schema);

}  // namespace loom::encoding

#endif  // LOOMCXX_ENCODING_H_
