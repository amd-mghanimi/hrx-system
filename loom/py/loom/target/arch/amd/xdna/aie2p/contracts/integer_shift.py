# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Packed integer shift contracts for AMD XDNA AIE2P."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal, cast

from loom.dialect.vector import defs as vector
from loom.dsl import Op
from loom.target.arch.amd.xdna.aie2p.contracts.packet_program import (
    IntegerSignedness,
    PacketProgram,
    PairPacketProgram,
    narrow_integer_packet,
    shift_integer_packet,
    shift_integer_packet_fixed,
    widen_integer_packet,
)
from loom.target.contracts import (
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    ValueProject,
    ValueRef,
    Vector,
)

ShiftDirection = Literal["left", "right"]
WordPair = tuple[ValueRef, ValueRef]


@dataclass(frozen=True, slots=True)
class IntegerShiftOperation:
    """One source shift operation and its packet arithmetic semantics."""

    source_op: Op
    direction: ShiftDirection
    signedness: IntegerSignedness


INTEGER_SHIFT_OPERATIONS = (
    IntegerShiftOperation(vector.vector_shli, "left", "unsigned"),
    IntegerShiftOperation(vector.vector_shrui, "right", "unsigned"),
    IntegerShiftOperation(vector.vector_shrsi, "right", "signed"),
)


@dataclass(frozen=True, slots=True)
class IntegerShiftRuleShape:
    """Logical element interval realized by one physical 512-bit packet."""

    element_bits: int
    minimum_element_count: int
    maximum_element_count: int

    def __post_init__(self) -> None:
        native_element_count = 512 // self.element_bits
        if not (
            self.element_bits in (8, 16, 32, 64)
            and 1
            <= self.minimum_element_count
            <= self.maximum_element_count
            <= native_element_count
        ):
            raise ValueError("integer shift logical element interval is invalid")

    @property
    def native_element_count(self) -> int:
        """Number of source elements carried by one physical packet."""

        return 512 // self.element_bits

    @property
    def vector_type(self) -> Vector:
        """Source-visible packet type interval across all static ranks."""

        return Vector(
            f"i{self.element_bits}",
            minimum_static_elements=self.minimum_element_count,
            maximum_static_elements=self.maximum_element_count,
        )


# Full and partial rules let compile reports distinguish a complete physical
# packet while admitting any static vector rank whose total elements fit it.
INTEGER_SHIFT_RULE_SHAPES = tuple(
    IntegerShiftRuleShape(element_bits, minimum, maximum)
    for element_bits in (8, 16, 32, 64)
    for minimum, maximum in (
        (512 // element_bits, 512 // element_bits),
        (1, 512 // element_bits - 1),
    )
)


def _typed_guards(rule_shape: IntegerShiftRuleShape) -> tuple[Guard, ...]:
    return tuple(
        Guard.value_type(field, rule_shape.vector_type)
        for field in ("lhs", "rhs", "result")
    )


def _rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
    program: PacketProgram,
    *,
    guards: tuple[Guard, ...] = (),
    priority: int = 0,
    report_key: str,
) -> DescriptorRule:
    final_emit = cast(EmitDescriptorOp, program.emits[-1])
    return DescriptorRule(
        source_op=operation.source_op,
        descriptor=final_emit.descriptor,
        guards=(*_typed_guards(rule_shape), *guards),
        emit=program.emits,
        priority=priority,
        report_key=report_key,
    )


def _projected_shift(
    program: PacketProgram,
    name: str,
    value: ValueProject,
) -> ValueRef:
    """Materializes one exact source fact directly in the shift register."""

    return program.constant(
        name,
        value,
        descriptor_key="amd.xdna.aie2p.constant.i32.shift",
    )


def _adjusted_projected_shift(
    program: PacketProgram,
    name: str,
    value: ValueProject,
    adjustment: int,
) -> ValueRef:
    """Adds an immediate to one exact source fact into the shift register."""

    scalar = program.constant(
        f"{name}_scalar",
        value,
        descriptor_key="amd.xdna.aie2p.constant.i32.short",
    )
    return program.operation(
        name,
        "add.i32.immediate.shift",
        "dst",
        immediates={"imm": adjustment},
        s0=scalar,
    )


def _uniform_native_rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> DescriptorRule:
    """Shifts one exact uniform i8/i16/i32 packet through accumulators."""

    program = PacketProgram(rule_shape.element_bits)
    if operation.direction == "left":
        upshift = _projected_shift(program, "distance", ValueProject.exact_i64("rhs"))
        downshift = program.shift(0)
    else:
        upshift = program.shift(0)
        downshift = _projected_shift(program, "distance", ValueProject.exact_i64("rhs"))
    shift_integer_packet(
        program,
        None,
        ValueRef.operand("lhs"),
        upshift,
        downshift,
        signedness=operation.signedness,
    )
    return _rule(
        operation,
        rule_shape,
        program,
        guards=(
            Guard.value_exact_i64("rhs"),
            Guard.value_i64_range("rhs", 0, rule_shape.element_bits - 1),
        ),
        priority=1,
        report_key="integer_shift.uniform.accumulator",
    )


def _varying_native_rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> DescriptorRule:
    """Builds a logarithmic per-lane barrel from fixed native shifts."""

    program = PacketProgram(rule_shape.element_bits)
    current = ValueRef.operand("lhs")
    rhs = ValueRef.operand("rhs")
    stage_amounts = tuple(
        1 << bit for bit in range(rule_shape.element_bits.bit_length() - 1)
    )
    mask = program.splat("stage_1_mask", 1)
    for stage_index, amount in enumerate(stage_amounts):
        candidate = shift_integer_packet_fixed(
            program,
            f"stage_{amount}_candidate",
            current,
            amount if operation.direction == "left" else -amount,
            signedness=operation.signedness,
        )
        selected_bit = program.binary(
            f"stage_{amount}_selected_bit", "and.bits512", rhs, mask
        )
        inactive = program.compare_zero(f"stage_{amount}_inactive", selected_bit)
        current = program.select(
            None if stage_index == len(stage_amounts) - 1 else f"stage_{amount}_result",
            current,
            candidate,
            inactive,
        )
        if stage_index != len(stage_amounts) - 1:
            next_amount = stage_amounts[stage_index + 1]
            mask = program.binary(
                f"stage_{next_amount}_mask",
                f"add.i{rule_shape.element_bits}x{rule_shape.native_element_count}",
                mask,
                mask,
            )
    return _rule(
        operation,
        rule_shape,
        program,
        report_key="integer_shift.varying.barrel",
    )


def _shift_i64_words_left(
    program: PairPacketProgram,
    words: WordPair,
    distance: ValueRef,
    prefix: str,
) -> WordPair:
    """Shifts deinterleaved i64 words left by a count in [1, 31]."""

    zero = program.shift(0)
    word_bits = program.shift(32)
    low_wide = widen_integer_packet(program, f"{prefix}_low_wide", words[0], distance)
    low = narrow_integer_packet(program, f"{prefix}_low", low_wide, zero)
    carry = narrow_integer_packet(program, f"{prefix}_carry", low_wide, word_bits)
    high_wide = widen_integer_packet(program, f"{prefix}_high_wide", words[1], distance)
    high = narrow_integer_packet(program, f"{prefix}_high_shifted", high_wide, zero)
    high = program.binary(f"{prefix}_high", "or.bits512", high, carry)
    return low, high


def _shift_i64_words_right(
    program: PairPacketProgram,
    words: WordPair,
    complement: ValueRef,
    prefix: str,
    *,
    signedness: IntegerSignedness,
) -> WordPair:
    """Shifts deinterleaved i64 words right by 32-complement bits."""

    zero = program.shift(0)
    word_bits = program.shift(32)
    low_wide = widen_integer_packet(program, f"{prefix}_low_wide", words[0], complement)
    low = narrow_integer_packet(program, f"{prefix}_low_shifted", low_wide, word_bits)
    high_wide = widen_integer_packet(
        program,
        f"{prefix}_high_wide",
        words[1],
        complement,
        signedness=signedness,
    )
    carry = narrow_integer_packet(
        program,
        f"{prefix}_carry",
        high_wide,
        zero,
        signedness=signedness,
    )
    low = program.binary(f"{prefix}_low", "or.bits512", low, carry)
    high = narrow_integer_packet(
        program,
        f"{prefix}_high",
        high_wide,
        word_bits,
        signedness=signedness,
    )
    return low, high


def _shift_i64_words_fixed(
    program: PairPacketProgram,
    words: WordPair,
    amount: int,
    operation: IntegerShiftOperation,
    prefix: str,
) -> WordPair:
    if not 1 <= amount <= 31:
        raise ValueError("i64 word shift must be between one and 31 bits")
    if operation.direction == "left":
        return _shift_i64_words_left(program, words, program.shift(amount), prefix)
    return _shift_i64_words_right(
        program,
        words,
        program.shift(32 - amount),
        prefix,
        signedness=operation.signedness,
    )


def _shift_i64_words_by_32(
    program: PairPacketProgram,
    words: WordPair,
    operation: IntegerShiftOperation,
    prefix: str,
) -> WordPair:
    """Returns the fixed 32-bit stage of an i64 barrel."""

    zero = program.binary(f"{prefix}_zero", "sub.i32x16", words[0], words[0])
    if operation.direction == "left":
        return zero, words[0]
    if operation.signedness == "unsigned":
        return words[1], zero
    sign = shift_integer_packet_fixed(
        program,
        f"{prefix}_sign",
        words[1],
        -31,
        signedness="signed",
    )
    return words[1], sign


def _uniform_i64_zero_rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> DescriptorRule:
    program = PacketProgram(32)
    lhs = ValueRef.operand("lhs")
    program.binary(None, "or.bits512", lhs, lhs)
    return _rule(
        operation,
        rule_shape,
        program,
        guards=(Guard.value_exact_i64("rhs"), Guard.value_i64_range("rhs", 0, 0)),
        priority=1,
        report_key="integer_shift.uniform.i64_words",
    )


def _uniform_i64_low_rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> DescriptorRule:
    program = PairPacketProgram()
    words = program.split_i64_words(ValueRef.operand("lhs"), "lhs")
    if operation.direction == "left":
        distance = _projected_shift(program, "distance", ValueProject.exact_i64("rhs"))
        result = _shift_i64_words_left(program, words, distance, "shift")
    else:
        complement = _adjusted_projected_shift(
            program,
            "complement",
            ValueProject.exact_i64_negate("rhs"),
            32,
        )
        result = _shift_i64_words_right(
            program,
            words,
            complement,
            "shift",
            signedness=operation.signedness,
        )
    program.join_i64_words(*result, None)
    return _rule(
        operation,
        rule_shape,
        program,
        guards=(Guard.value_exact_i64("rhs"), Guard.value_i64_range("rhs", 1, 31)),
        priority=1,
        report_key="integer_shift.uniform.i64_words",
    )


def _uniform_i64_high_rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> DescriptorRule:
    program = PairPacketProgram()
    words = program.split_i64_words(ValueRef.operand("lhs"), "lhs")
    distance = _adjusted_projected_shift(
        program,
        "distance",
        ValueProject.exact_i64("rhs"),
        -32,
    )
    zero_shift = program.shift(0)
    zero_words = program.binary("zero_words", "sub.i32x16", words[0], words[0])
    if operation.direction == "left":
        result = (
            zero_words,
            shift_integer_packet(
                program,
                "shifted_high",
                words[0],
                distance,
                zero_shift,
            ),
        )
    else:
        low = shift_integer_packet(
            program,
            "shifted_low",
            words[1],
            zero_shift,
            distance,
            signedness=operation.signedness,
        )
        high = zero_words
        if operation.signedness == "signed":
            high = shift_integer_packet_fixed(
                program,
                "sign_words",
                words[1],
                -31,
                signedness="signed",
            )
        result = low, high
    program.join_i64_words(*result, None)
    return _rule(
        operation,
        rule_shape,
        program,
        guards=(
            Guard.value_exact_i64("rhs"),
            Guard.value_i64_range("rhs", 32, 63),
        ),
        priority=1,
        report_key="integer_shift.uniform.i64_words",
    )


def _varying_i64_rule(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> DescriptorRule:
    """Builds a six-stage per-lane barrel over deinterleaved i32 words."""

    program = PairPacketProgram()
    current = program.split_i64_words(ValueRef.operand("lhs"), "lhs")
    counts = program.select_i64_word(ValueRef.operand("rhs"), "rhs_low_words", 0)
    stage_amounts = (1, 2, 4, 8, 16, 32)
    mask = program.splat("stage_1_mask", 1)
    for stage_index, amount in enumerate(stage_amounts[:-1]):
        candidate = _shift_i64_words_fixed(
            program, current, amount, operation, f"stage_{amount}"
        )
        selected_bit = program.binary(
            f"stage_{amount}_selected_bit", "and.bits512", counts, mask
        )
        inactive = program.compare_zero(f"stage_{amount}_inactive", selected_bit)
        current = (
            program.select(
                f"stage_{amount}_selected_low", current[0], candidate[0], inactive
            ),
            program.select(
                f"stage_{amount}_selected_high", current[1], candidate[1], inactive
            ),
        )
        next_amount = stage_amounts[stage_index + 1]
        mask = program.binary(f"stage_{next_amount}_mask", "add.i32x16", mask, mask)
    candidate = _shift_i64_words_by_32(program, current, operation, "stage_32")
    selected_bit = program.binary("stage_32_selected_bit", "and.bits512", counts, mask)
    inactive = program.compare_zero("stage_32_inactive", selected_bit)
    result = (
        program.select("stage_32_low", current[0], candidate[0], inactive),
        program.select("stage_32_high", current[1], candidate[1], inactive),
    )
    program.join_i64_words(*result, None)
    return _rule(
        operation,
        rule_shape,
        program,
        report_key="integer_shift.varying.barrel",
    )


def _integer_shift_rules(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> tuple[DescriptorRule, ...]:
    if rule_shape.element_bits != 64:
        return (
            _uniform_native_rule(operation, rule_shape),
            _varying_native_rule(operation, rule_shape),
        )
    return (
        _uniform_i64_zero_rule(operation, rule_shape),
        _uniform_i64_low_rule(operation, rule_shape),
        _uniform_i64_high_rule(operation, rule_shape),
        _varying_i64_rule(operation, rule_shape),
    )


AIE2P_INTEGER_SHIFT_RULES = tuple(
    rule
    for operation in INTEGER_SHIFT_OPERATIONS
    for rule_shape in INTEGER_SHIFT_RULE_SHAPES
    for rule in _integer_shift_rules(operation, rule_shape)
)
