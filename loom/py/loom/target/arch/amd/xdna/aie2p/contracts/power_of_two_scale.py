# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact floating power-of-two scaling contracts for AMD XDNA AIE2P."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Literal

from loom.dialect.scalar import arithmetic as scalar_arithmetic
from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.conversion import (
    emit_f16_to_f32,
    emit_f32_to_f16,
)
from loom.target.arch.amd.xdna.aie2p.contracts.packet_program import (
    PacketProgram,
    shift_integer_packet_fixed,
)
from loom.target.arch.amd.xdna.aie2p.contracts.scalar_program import ScalarProgram
from loom.target.contracts import (
    DescriptorRule,
    Guard,
    Scalar,
    TypePattern,
    ValueProject,
    ValueRef,
    Vector,
)

_F16 = Scalar("f16")
_F32 = Scalar("f32")
_F32_PACKET = Vector("f32", minimum_static_elements=1, maximum_static_elements=16)
_ExponentDirection = Literal["negative", "nonnegative"]

_F32_ABSOLUTE_MASK = 0x7FFFFFFF
_F32_SIGN_MASK = -(1 << 31)
_F32_FRACTION_MASK = 0x007FFFFF
_F32_IMPLICIT_BIT = 0x00800000
_F32_INFINITY = 0x7F800000
_F32_QUIET_BIT = 0x00400000
_F32_MANTISSA_BITS = 23
_F32_MAXIMUM_ROUND_SHIFT = _F32_MANTISSA_BITS + 2


def _scalar_round_shift(
    program: ScalarProgram,
    value: ValueRef,
    amount: ValueRef,
) -> ValueRef:
    """Rounds an unsigned i32 right shift to nearest, ties to even."""

    zero = program.constant("round_zero", 0)
    one = program.constant("round_one", 1)
    negative_amount = program.binary("round_negative_amount", "sub.i32", zero, amount)
    quotient = program.binary("round_quotient", "lshl.i32", value, negative_amount)
    quotient_lsb = program.binary("round_quotient_lsb", "and.i32", quotient, one)
    power = program.binary("round_power", "lshl.i32", one, amount)
    negative_one = program.constant("round_negative_one", -1)
    half = program.binary("round_half", "lshl.i32", power, negative_one)
    bias_base = program.binary("round_bias_base", "sub.i32", half, one)
    bias = program.binary("round_bias", "add.i32", bias_base, quotient_lsb)
    biased_value = program.binary("round_biased_value", "add.i32", value, bias)
    return program.binary("round_result", "lshl.i32", biased_value, negative_amount)


def _scalar_f32_parts(
    program: ScalarProgram,
    value: ValueRef,
) -> tuple[ValueRef, ValueRef, ValueRef]:
    """Returns magnitude, encoded exponent, and fraction for binary32 bits."""

    absolute_mask = program.constant("absolute_mask", _F32_ABSOLUTE_MASK)
    magnitude = program.binary("magnitude", "and.i32", value, absolute_mask)
    exponent_shift = program.constant("exponent_shift", -_F32_MANTISSA_BITS)
    encoded_exponent = program.binary(
        "encoded_exponent", "lshl.i32", magnitude, exponent_shift
    )
    fraction_mask = program.constant("fraction_mask", _F32_FRACTION_MASK)
    fraction = program.binary("fraction", "and.i32", magnitude, fraction_mask)
    return magnitude, encoded_exponent, fraction


def _finish_scalar_f32_scale(
    program: ScalarProgram,
    value: ValueRef,
    magnitude: ValueRef,
    finite_magnitude: ValueRef,
    factor_bits: ValueProject,
    factor_bit_shift: int,
    result_name: str | None,
) -> ValueRef:
    """Adds the product sign and exact binary32 special-value behavior."""

    factor = program.constant("factor_bits", factor_bits)
    if factor_bit_shift:
        bit_shift = program.constant("factor_bit_shift", factor_bit_shift)
        factor = program.binary("positioned_factor_bits", "lshl.i32", factor, bit_shift)
    sign_xor = program.binary("sign_xor", "xor.i32", value, factor)
    sign_mask = program.constant("sign_mask", _F32_SIGN_MASK)
    product_sign = program.binary("product_sign", "and.i32", sign_xor, sign_mask)
    finite = program.binary("signed_finite", "or.i32", finite_magnitude, product_sign)

    infinity = program.constant("infinity", _F32_INFINITY)
    is_nan = program.binary("is_nan", "cmp.ult.i32", infinity, magnitude)
    quiet_bit = program.constant("quiet_bit", _F32_QUIET_BIT)
    quiet_nan = program.binary("quiet_nan", "or.i32", value, quiet_bit)
    signed_infinity = program.binary(
        "signed_infinity", "or.i32", infinity, product_sign
    )
    special = program.select("special", quiet_nan, signed_infinity, is_nan)
    is_special = program.binary("is_special", "cmp.uge.i32", magnitude, infinity)
    return program.select(result_name, special, finite, is_special)


def _emit_scalar_f32_negative_scale(
    program: ScalarProgram,
    value: ValueRef,
    factor_field: str,
    factor_bits: ValueProject,
    factor_bit_shift: int,
    result_name: str | None,
) -> ValueRef:
    """Emits exact binary32 multiplication by 2^-q for q in [1, 149]."""

    magnitude, encoded_exponent, fraction = _scalar_f32_parts(program, value)
    one = program.constant("one", 1)
    zero = program.constant("zero", 0)
    implicit_bit = program.constant("implicit_bit", _F32_IMPLICIT_BIT)
    exponent_is_zero = program.unary(
        "exponent_is_zero", "cmp.eqz.i32", encoded_exponent
    )
    selected_implicit_bit = program.select(
        "selected_implicit_bit", zero, implicit_bit, exponent_is_zero
    )
    significand = program.binary(
        "significand", "or.i32", fraction, selected_implicit_bit
    )

    distance = program.constant(
        "distance",
        ValueProject.float_power_of_two_negated_exponent(factor_field),
        descriptor_key="amd.xdna.aie2p.constant.i32.short",
    )
    minimum_exponent = program.select(
        "minimum_exponent", one, encoded_exponent, exponent_is_zero
    )
    below_distance = program.binary(
        "below_distance", "cmp.ult.i32", minimum_exponent, distance
    )
    clamped_exponent = program.select(
        "clamped_exponent", minimum_exponent, distance, below_distance
    )
    distance_plus_one = program.add_immediate("distance_plus_one", distance, 1)
    round_shift = program.binary(
        "round_shift", "sub.i32", distance_plus_one, clamped_exponent
    )
    maximum_shift = program.constant("maximum_round_shift", _F32_MAXIMUM_ROUND_SHIFT)
    shift_too_large = program.binary(
        "shift_too_large", "cmp.ult.i32", maximum_shift, round_shift
    )
    round_shift = program.select(
        "clamped_round_shift", maximum_shift, round_shift, shift_too_large
    )
    underflow = _scalar_round_shift(program, significand, round_shift)

    exponent_bit_shift = program.constant("exponent_bit_shift", _F32_MANTISSA_BITS)
    exponent_delta = program.binary(
        "exponent_delta", "lshl.i32", distance, exponent_bit_shift
    )
    normal = program.binary("normal", "sub.i32", magnitude, exponent_delta)
    normal_threshold_exponent = program.binary(
        "normal_threshold_exponent",
        "lshl.i32",
        distance_plus_one,
        exponent_bit_shift,
    )
    is_normal = program.binary(
        "is_normal", "cmp.uge.i32", magnitude, normal_threshold_exponent
    )
    finite = program.select("finite", normal, underflow, is_normal)
    return _finish_scalar_f32_scale(
        program,
        value,
        magnitude,
        finite,
        factor_bits,
        factor_bit_shift,
        result_name,
    )


def _emit_scalar_f32_nonnegative_scale(
    program: ScalarProgram,
    value: ValueRef,
    factor_field: str,
    factor_bits: ValueProject,
    factor_bit_shift: int,
    result_name: str | None,
) -> ValueRef:
    """Emits exact binary32 multiplication by 2^k for k in [0, 127]."""

    magnitude, encoded_exponent, fraction = _scalar_f32_parts(program, value)
    leading_zeros = program.unary("leading_zeros", "clz.i32", fraction)
    normalization_shift = program.add_immediate(
        "normalization_shift", leading_zeros, -8
    )
    normalized = program.binary("normalized", "lshl.i32", fraction, normalization_shift)
    implicit_bit = program.constant("implicit_bit", _F32_IMPLICIT_BIT)
    normal_significand = program.binary(
        "normal_significand", "or.i32", fraction, implicit_bit
    )
    exponent_is_zero = program.unary(
        "exponent_is_zero", "cmp.eqz.i32", encoded_exponent
    )
    significand = program.select(
        "significand", normalized, normal_significand, exponent_is_zero
    )

    one = program.constant("one", 1)
    subnormal_exponent = program.binary(
        "subnormal_exponent", "sub.i32", one, normalization_shift
    )
    effective_exponent = program.select(
        "effective_exponent",
        subnormal_exponent,
        encoded_exponent,
        exponent_is_zero,
    )
    scale_exponent = program.constant(
        "scale_exponent",
        ValueProject.float_power_of_two_exponent(factor_field),
        descriptor_key="amd.xdna.aie2p.constant.i32.short",
    )
    result_exponent = program.binary(
        "result_exponent", "add.i32", effective_exponent, scale_exponent
    )
    is_underflow = program.binary("is_underflow", "cmp.slt.i32", result_exponent, one)
    raw_round_shift = program.binary("raw_round_shift", "sub.i32", one, result_exponent)
    round_shift = program.select(
        "positive_round_shift", raw_round_shift, one, is_underflow
    )
    maximum_shift = program.constant("maximum_round_shift", _F32_MAXIMUM_ROUND_SHIFT)
    shift_too_large = program.binary(
        "shift_too_large", "cmp.ult.i32", maximum_shift, round_shift
    )
    round_shift = program.select(
        "clamped_round_shift", maximum_shift, round_shift, shift_too_large
    )
    underflow = _scalar_round_shift(program, significand, round_shift)

    exponent_bit_shift = program.constant("exponent_bit_shift", _F32_MANTISSA_BITS)
    exponent_bits = program.binary(
        "exponent_bits", "lshl.i32", result_exponent, exponent_bit_shift
    )
    fraction_mask = program.constant("result_fraction_mask", _F32_FRACTION_MASK)
    result_fraction = program.binary(
        "result_fraction", "and.i32", significand, fraction_mask
    )
    normal = program.binary("normal", "or.i32", exponent_bits, result_fraction)
    finite = program.select("finite_range", underflow, normal, is_underflow)

    maximum_finite_exponent = program.constant("maximum_finite_exponent", 254)
    is_overflow = program.binary(
        "is_overflow",
        "cmp.slt.i32",
        maximum_finite_exponent,
        result_exponent,
    )
    infinity = program.constant("overflow_infinity", _F32_INFINITY)
    finite = program.select("finite_or_infinity", infinity, finite, is_overflow)
    zero = program.constant("zero", 0)
    is_zero = program.unary("is_zero", "cmp.eqz.i32", magnitude)
    finite = program.select("finite", zero, finite, is_zero)
    return _finish_scalar_f32_scale(
        program,
        value,
        magnitude,
        finite,
        factor_bits,
        factor_bit_shift,
        result_name,
    )


def _emit_scalar_f32_scale(
    program: ScalarProgram,
    value: ValueRef,
    factor_field: str,
    factor_bits: ValueProject,
    factor_bit_shift: int,
    direction: _ExponentDirection,
    result_name: str | None,
) -> ValueRef:
    if direction == "negative":
        return _emit_scalar_f32_negative_scale(
            program,
            value,
            factor_field,
            factor_bits,
            factor_bit_shift,
            result_name,
        )
    return _emit_scalar_f32_nonnegative_scale(
        program,
        value,
        factor_field,
        factor_bits,
        factor_bit_shift,
        result_name,
    )


def _packet_binary(
    program: PacketProgram,
    name: str,
    operation: str,
    lhs: ValueRef,
    rhs: ValueRef,
) -> ValueRef:
    return program.binary(name, f"{operation}.i32x16", lhs, rhs)


def _packet_bits(
    program: PacketProgram,
    name: str,
    operation: str,
    lhs: ValueRef,
    rhs: ValueRef,
) -> ValueRef:
    return program.binary(name, f"{operation}.bits512", lhs, rhs)


def _packet_shift_conditions(
    program: PacketProgram,
    amount: ValueRef,
    bit_values: Mapping[int, ValueRef],
) -> tuple[tuple[int, ValueRef], ...]:
    conditions = []
    for bit in (1, 2, 4, 8, 16):
        selected_bit = _packet_bits(
            program,
            f"round_shift_bit_{bit}",
            "and",
            amount,
            bit_values[bit],
        )
        inactive = program.compare_zero(f"round_shift_bit_{bit}_inactive", selected_bit)
        conditions.append((bit, inactive))
    return tuple(conditions)


def _packet_variable_shift(
    program: PacketProgram,
    name: str,
    value: ValueRef,
    conditions: Sequence[tuple[int, ValueRef]],
    *,
    left: bool,
) -> ValueRef:
    shifted = value
    for bit, inactive in conditions:
        candidate = shift_integer_packet_fixed(
            program,
            f"{name}_by_{bit}",
            shifted,
            bit if left else -bit,
        )
        shifted = program.select(f"{name}_selected_{bit}", shifted, candidate, inactive)
    return shifted


def _packet_round_shift(
    program: PacketProgram,
    value: ValueRef,
    amount: ValueRef,
    one: ValueRef,
    bit_values: Mapping[int, ValueRef],
) -> ValueRef:
    """Rounds lane-varying unsigned right shifts to nearest, ties to even."""

    conditions = _packet_shift_conditions(program, amount, bit_values)
    quotient = _packet_variable_shift(
        program, "round_quotient", value, conditions, left=False
    )
    quotient_lsb = _packet_bits(program, "round_quotient_lsb", "and", quotient, one)
    power = _packet_variable_shift(program, "round_power", one, conditions, left=True)
    half = shift_integer_packet_fixed(program, "round_half", power, -1)
    bias_base = _packet_binary(program, "round_bias_base", "sub", half, one)
    bias = _packet_binary(program, "round_bias", "add", bias_base, quotient_lsb)
    biased_value = _packet_binary(program, "round_biased_value", "add", value, bias)
    return _packet_variable_shift(
        program, "round_result", biased_value, conditions, left=False
    )


def _packet_f32_parts(
    program: PacketProgram,
    value: ValueRef,
) -> tuple[ValueRef, ValueRef, ValueRef]:
    absolute_mask = program.splat("absolute_mask", _F32_ABSOLUTE_MASK)
    magnitude = _packet_bits(program, "magnitude", "and", value, absolute_mask)
    encoded_exponent = shift_integer_packet_fixed(
        program, "encoded_exponent", magnitude, -_F32_MANTISSA_BITS
    )
    fraction_mask = program.splat("fraction_mask", _F32_FRACTION_MASK)
    fraction = _packet_bits(program, "fraction", "and", magnitude, fraction_mask)
    return magnitude, encoded_exponent, fraction


def _finish_packet_f32_scale(
    program: PacketProgram,
    value: ValueRef,
    magnitude: ValueRef,
    finite_magnitude: ValueRef,
    factor_field: str,
) -> ValueRef:
    factor = program.splat("factor_bits", ValueProject.float_as_f32_i32(factor_field))
    sign_mask = program.splat("sign_mask", _F32_SIGN_MASK)
    value_sign = _packet_bits(program, "value_sign", "and", value, sign_mask)
    factor_sign = _packet_bits(program, "factor_sign", "and", factor, sign_mask)
    product_sign = _packet_binary(
        program, "product_sign", "add", value_sign, factor_sign
    )
    finite = _packet_bits(
        program, "signed_finite", "or", finite_magnitude, product_sign
    )

    infinity = program.splat("infinity", _F32_INFINITY)
    is_nan = program.compare_unsigned_less_than("is_nan", infinity, magnitude)
    quiet_bit = program.splat("quiet_bit", _F32_QUIET_BIT)
    quiet_nan = _packet_bits(program, "quiet_nan", "or", value, quiet_bit)
    signed_infinity = _packet_bits(
        program, "signed_infinity", "or", infinity, product_sign
    )
    special = program.select("special", quiet_nan, signed_infinity, is_nan)
    is_special = program.compare_unsigned_greater_equal(
        "is_special", magnitude, infinity
    )
    return program.select(None, special, finite, is_special)


def _packet_common_splats(
    program: PacketProgram,
) -> tuple[ValueRef, ValueRef, dict[int, ValueRef]]:
    zero = program.splat("zero", 0)
    one = program.splat("one", 1)
    bit_values = {
        bit: program.splat(f"shift_bit_{bit}", bit) for bit in (1, 2, 4, 8, 16)
    }
    return zero, one, bit_values


def _emit_packet_f32_negative_scale(
    program: PacketProgram,
    value: ValueRef,
    factor_field: str,
) -> ValueRef:
    magnitude, encoded_exponent, fraction = _packet_f32_parts(program, value)
    zero, one, bit_values = _packet_common_splats(program)
    implicit_bit = program.splat("implicit_bit", _F32_IMPLICIT_BIT)
    exponent_is_zero = program.compare_zero("exponent_is_zero", encoded_exponent)
    selected_implicit_bit = program.select(
        "selected_implicit_bit", zero, implicit_bit, exponent_is_zero
    )
    significand = _packet_bits(
        program, "significand", "or", fraction, selected_implicit_bit
    )

    distance = program.splat(
        "distance",
        ValueProject.float_power_of_two_negated_exponent(factor_field),
        descriptor_key="amd.xdna.aie2p.constant.i32.short",
    )
    minimum_exponent = program.select(
        "minimum_exponent", one, encoded_exponent, exponent_is_zero
    )
    below_distance = program.compare_unsigned_less_than(
        "below_distance", minimum_exponent, distance
    )
    clamped_exponent = program.select(
        "clamped_exponent", minimum_exponent, distance, below_distance
    )
    distance_plus_one = _packet_binary(
        program, "distance_plus_one", "add", distance, one
    )
    round_shift = _packet_binary(
        program, "round_shift", "sub", distance_plus_one, clamped_exponent
    )
    maximum_shift = program.splat("maximum_round_shift", _F32_MAXIMUM_ROUND_SHIFT)
    shift_too_large = program.compare_unsigned_less_than(
        "shift_too_large", maximum_shift, round_shift
    )
    round_shift = program.select(
        "clamped_round_shift", maximum_shift, round_shift, shift_too_large
    )
    underflow = _packet_round_shift(program, significand, round_shift, one, bit_values)

    exponent_delta = shift_integer_packet_fixed(
        program, "exponent_delta", distance, _F32_MANTISSA_BITS
    )
    normal = _packet_binary(program, "normal", "sub", magnitude, exponent_delta)
    normal_threshold_exponent = shift_integer_packet_fixed(
        program,
        "normal_threshold_exponent",
        distance_plus_one,
        _F32_MANTISSA_BITS,
    )
    is_normal = program.compare_unsigned_greater_equal(
        "is_normal", magnitude, normal_threshold_exponent
    )
    finite = program.select("finite", normal, underflow, is_normal)
    return _finish_packet_f32_scale(program, value, magnitude, finite, factor_field)


def _emit_packet_f32_nonnegative_scale(
    program: PacketProgram,
    value: ValueRef,
    factor_field: str,
) -> ValueRef:
    magnitude, encoded_exponent, fraction = _packet_f32_parts(program, value)
    zero, one, bit_values = _packet_common_splats(program)

    normalized = fraction
    normalization_shift = zero
    for bit in (16, 8, 4, 2, 1):
        threshold = program.splat(f"normalize_threshold_{bit}", 1 << (24 - bit))
        needs_shift = program.compare_unsigned_less_than(
            f"normalize_by_{bit}", normalized, threshold
        )
        candidate = shift_integer_packet_fixed(
            program, f"normalized_by_{bit}", normalized, bit
        )
        normalized = program.select(
            f"normalized_selected_{bit}", candidate, normalized, needs_shift
        )
        shift_candidate = _packet_binary(
            program,
            f"normalization_shift_plus_{bit}",
            "add",
            normalization_shift,
            bit_values[bit],
        )
        normalization_shift = program.select(
            f"normalization_shift_selected_{bit}",
            shift_candidate,
            normalization_shift,
            needs_shift,
        )
    fraction_is_zero = program.compare_zero("fraction_is_zero", fraction)
    zero_normalization_shift = program.splat("zero_normalization_shift", 24)
    normalization_shift = program.select(
        "normalization_shift",
        zero_normalization_shift,
        normalization_shift,
        fraction_is_zero,
    )

    implicit_bit = program.splat("implicit_bit", _F32_IMPLICIT_BIT)
    normal_significand = _packet_bits(
        program, "normal_significand", "or", fraction, implicit_bit
    )
    exponent_is_zero = program.compare_zero("exponent_is_zero", encoded_exponent)
    significand = program.select(
        "significand", normalized, normal_significand, exponent_is_zero
    )
    subnormal_exponent = _packet_binary(
        program, "subnormal_exponent", "sub", one, normalization_shift
    )
    effective_exponent = program.select(
        "effective_exponent",
        subnormal_exponent,
        encoded_exponent,
        exponent_is_zero,
    )
    scale_exponent = program.splat(
        "scale_exponent",
        ValueProject.float_power_of_two_exponent(factor_field),
        descriptor_key="amd.xdna.aie2p.constant.i32.short",
    )
    result_exponent = _packet_binary(
        program, "result_exponent", "add", effective_exponent, scale_exponent
    )
    is_underflow = program.compare_signed_less_than(
        "is_underflow", result_exponent, one
    )
    raw_round_shift = _packet_binary(
        program, "raw_round_shift", "sub", one, result_exponent
    )
    round_shift = program.select(
        "positive_round_shift", raw_round_shift, one, is_underflow
    )
    maximum_shift = program.splat("maximum_round_shift", _F32_MAXIMUM_ROUND_SHIFT)
    shift_too_large = program.compare_unsigned_less_than(
        "shift_too_large", maximum_shift, round_shift
    )
    round_shift = program.select(
        "clamped_round_shift", maximum_shift, round_shift, shift_too_large
    )
    underflow = _packet_round_shift(program, significand, round_shift, one, bit_values)

    exponent_bits = shift_integer_packet_fixed(
        program, "exponent_bits", result_exponent, _F32_MANTISSA_BITS
    )
    result_fraction = _packet_bits(
        program,
        "result_fraction",
        "and",
        significand,
        program.splat("result_fraction_mask", _F32_FRACTION_MASK),
    )
    normal = _packet_bits(program, "normal", "or", exponent_bits, result_fraction)
    finite = program.select("finite_range", underflow, normal, is_underflow)
    maximum_finite_exponent = program.splat("maximum_finite_exponent", 254)
    is_overflow = program.compare_signed_less_than(
        "is_overflow", maximum_finite_exponent, result_exponent
    )
    infinity = program.splat("overflow_infinity", _F32_INFINITY)
    finite = program.select("finite_or_infinity", infinity, finite, is_overflow)
    is_zero = program.compare_zero("is_zero", magnitude)
    finite = program.select("finite", zero, finite, is_zero)
    return _finish_packet_f32_scale(program, value, magnitude, finite, factor_field)


def _scalar_scale_rule(
    element_type: TypePattern,
    factor_field: Literal["lhs", "rhs"],
    direction: _ExponentDirection,
) -> DescriptorRule:
    value_field = "rhs" if factor_field == "lhs" else "lhs"
    exponent_range = {
        ("f16", "negative"): (-24, -1),
        ("f16", "nonnegative"): (0, 15),
        ("f32", "negative"): (-149, -1),
        ("f32", "nonnegative"): (0, 127),
    }[(element_type.element, direction)]
    factor_bits = (
        ValueProject.float_bits(factor_field)
        if element_type == _F16
        else ValueProject.float_as_f32_i32(factor_field)
    )
    factor_bit_shift = 16 if element_type == _F16 else 0

    if element_type == _F16:
        widen_program = ScalarProgram("value_")
        value = emit_f16_to_f32(widen_program, ValueRef.operand(value_field), "wide")
        scale_program = ScalarProgram("scale_")
        scaled = _emit_scalar_f32_scale(
            scale_program,
            value,
            factor_field,
            factor_bits,
            factor_bit_shift,
            direction,
            "wide_result",
        )
        narrow_program = ScalarProgram("narrow_")
        emit_f32_to_f16(narrow_program, scaled, None)
        emits = (
            *widen_program.emits,
            *scale_program.emits,
            *narrow_program.emits,
        )
    else:
        scale_program = ScalarProgram()
        _emit_scalar_f32_scale(
            scale_program,
            ValueRef.operand(value_field),
            factor_field,
            factor_bits,
            factor_bit_shift,
            direction,
            None,
        )
        emits = tuple(scale_program.emits)

    return DescriptorRule(
        source_op=scalar_arithmetic.scalar_mulf,
        descriptor=emits[-1].descriptor,
        priority=1,
        guards=(
            *(
                Guard.value_type(field, element_type)
                for field in ("lhs", "rhs", "result")
            ),
            Guard.value_exact_power_of_two_float(
                factor_field, exponent_range[0], exponent_range[1]
            ),
        ),
        emit=emits,
        report_key="exact_power_of_two_float_scale",
    )


def _packet_scale_rule(
    factor_field: Literal["lhs", "rhs"],
    direction: _ExponentDirection,
) -> DescriptorRule:
    value_field = "rhs" if factor_field == "lhs" else "lhs"
    exponent_range = (-149, -1) if direction == "negative" else (0, 127)
    program = PacketProgram(32)
    if direction == "negative":
        _emit_packet_f32_negative_scale(
            program, ValueRef.operand(value_field), factor_field
        )
    else:
        _emit_packet_f32_nonnegative_scale(
            program, ValueRef.operand(value_field), factor_field
        )
    return DescriptorRule(
        source_op=vector.vector_mulf,
        descriptor=program.emits[-1].descriptor,
        priority=1,
        guards=(
            *(
                Guard.value_type(field, _F32_PACKET)
                for field in ("lhs", "rhs", "result")
            ),
            Guard.value_exact_power_of_two_float(
                factor_field, exponent_range[0], exponent_range[1]
            ),
        ),
        emit=tuple(program.emits),
        report_key="exact_power_of_two_float_scale",
    )


AIE2P_POWER_OF_TWO_SCALE_RULES = (
    *(
        _scalar_scale_rule(element_type, factor_field, direction)
        for element_type in (_F16, _F32)
        for direction in ("negative", "nonnegative")
        for factor_field in ("lhs", "rhs")
    ),
    *(
        _packet_scale_rule(factor_field, direction)
        for direction in ("negative", "nonnegative")
        for factor_field in ("lhs", "rhs")
    ),
)
