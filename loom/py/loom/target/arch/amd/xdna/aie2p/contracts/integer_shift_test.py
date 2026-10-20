# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Semantic tests for packed integer shifts on AMD XDNA AIE2P."""

from __future__ import annotations

from loom.target.arch.amd.xdna.aie2p.contracts.integer_shift import (
    AIE2P_INTEGER_SHIFT_RULES,
    INTEGER_SHIFT_OPERATIONS,
    INTEGER_SHIFT_RULE_SHAPES,
    IntegerShiftOperation,
    IntegerShiftRuleShape,
)
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    ValueProject,
    ValueProjectKind,
    ValueRef,
)

ScalarOrVector = int | tuple[int, ...]


def _mask(bits: int) -> int:
    return (1 << bits) - 1


def _signed(value: int, bits: int) -> int:
    value &= _mask(bits)
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _shape_rules(
    operation: IntegerShiftOperation,
    rule_shape: IntegerShiftRuleShape,
) -> tuple[DescriptorRule, ...]:
    typed_guards = tuple(
        Guard.value_type(field, rule_shape.vector_type)
        for field in ("lhs", "rhs", "result")
    )
    return tuple(
        rule
        for rule in AIE2P_INTEGER_SHIFT_RULES
        if rule.source_op is operation.source_op and rule.guards[:3] == typed_guards
    )


def _full_shape(element_bits: int) -> IntegerShiftRuleShape:
    shapes = [
        shape
        for shape in INTEGER_SHIFT_RULE_SHAPES
        if shape.element_bits == element_bits
        and shape.minimum_element_count == shape.native_element_count
    ]
    assert len(shapes) == 1
    return shapes[0]


def _uniform_rule(
    operation: IntegerShiftOperation, element_bits: int, count: int
) -> DescriptorRule:
    if element_bits != 64:
        count_range = (0, element_bits - 1)
    elif count == 0:
        count_range = (0, 0)
    elif count < 32:
        count_range = (1, 31)
    else:
        count_range = (32, 63)
    range_guard = Guard.value_i64_range("rhs", *count_range)
    rules = [
        rule
        for rule in _shape_rules(operation, _full_shape(element_bits))
        if range_guard in rule.guards
    ]
    assert len(rules) == 1
    return rules[0]


def _varying_rule(
    operation: IntegerShiftOperation, element_bits: int
) -> DescriptorRule:
    rules = [
        rule
        for rule in _shape_rules(operation, _full_shape(element_bits))
        if rule.report_key == "integer_shift.varying.barrel"
    ]
    assert len(rules) == 1
    return rules[0]


def _project(value: int | ValueProject, count: int) -> int:
    if isinstance(value, int):
        return value
    assert value.source_value == "rhs"
    if value.kind is ValueProjectKind.EXACT_I64:
        return count
    assert value.kind is ValueProjectKind.EXACT_I64_NEGATE
    return -count


def _splat_length(descriptor_key: str) -> int:
    return {
        "splat.i8x64": 64,
        "splat.i16x32": 32,
        "splat.i32x16": 16,
    }[descriptor_key]


def _integer_packet_bits(descriptor_key: str, state: dict[str, int]) -> int:
    if ".4x." in descriptor_key:
        assert (
            state["ups-mode" if descriptor_key.startswith("widen.") else "srs-mode"]
            == 0
        )
        return 8
    mode = state["ups-mode" if descriptor_key.startswith("widen.") else "srs-mode"]
    assert mode in (0, 1)
    return 16 if mode == 0 else 32


def _evaluate(
    rule: DescriptorRule,
    lhs: tuple[int, ...],
    rhs: tuple[int, ...],
    count: int,
) -> tuple[int, ...]:
    values: dict[ValueRef, ScalarOrVector] = {
        ValueRef.operand("lhs"): lhs,
        ValueRef.operand("rhs"): rhs,
    }
    state: dict[str, int] = {}
    for emit in rule.emit:
        assert isinstance(emit, EmitDescriptorOp)
        key = emit.descriptor.key.removeprefix("amd.xdna.aie2p.")
        if key.startswith("state."):
            state[key.removeprefix("state.").removesuffix(".immediate")] = (
                emit.immediates["i"]
            )
            continue
        operands = {name: values[ref] for name, ref in emit.operands.items()}
        if emit.form is DescriptorEmitForm.CONST:
            result: ScalarOrVector = _project(emit.immediates["i"], count)
        elif key == "add.i32.immediate.shift":
            assert isinstance(operands["s0"], int)
            result = operands["s0"] + emit.immediates["imm"]
        elif key.startswith("splat."):
            source = operands["src"]
            assert isinstance(source, int)
            result = (source,) * _splat_length(key)
        elif key.startswith(("add.i", "sub.i")):
            left = operands["s1"]
            right = operands["s2"]
            assert isinstance(left, tuple)
            assert isinstance(right, tuple)
            operation = int.__add__ if key.startswith("add.") else int.__sub__
            element_bits = 8 if "i8x64" in key else 16 if "i16x32" in key else 32
            result = tuple(
                operation(a, b) & _mask(element_bits)
                for a, b in zip(left, right, strict=True)
            )
        elif key in ("and.bits512", "or.bits512"):
            left = operands["s1"]
            right = operands["s2"]
            assert isinstance(left, tuple)
            assert isinstance(right, tuple)
            operation = int.__and__ if key == "and.bits512" else int.__or__
            result = tuple(operation(a, b) for a, b in zip(left, right, strict=True))
        elif key.startswith("cmp.eqz.i"):
            source = operands["s2"]
            assert isinstance(source, tuple)
            result = sum((value == 0) << index for index, value in enumerate(source))
        elif key == "predicate.complete.zero.high32":
            result = operands["storage"]
            assert isinstance(result, int)
        elif key.startswith("select.i"):
            false_values = operands["s1"]
            true_values = operands["s2"]
            predicate = operands["sel"]
            assert isinstance(false_values, tuple)
            assert isinstance(true_values, tuple)
            assert isinstance(predicate, int)
            result = tuple(
                true_value if predicate & (1 << index) else false_value
                for index, (true_value, false_value) in enumerate(
                    zip(true_values, false_values, strict=True)
                )
            )
        elif key == "shuffle.x.configured":
            first = operands["s1"]
            second = operands["s2"]
            control = operands["mod"]
            assert isinstance(first, tuple)
            assert isinstance(second, tuple)
            assert isinstance(control, int)
            if control in (4, 5):
                selected = first[(control - 4) :: 2]
                result = (*selected, *selected)
            else:
                assert control == 16
                result = tuple(
                    word
                    for pair in zip(first[:8], second[:8], strict=True)
                    for word in pair
                )
        elif key.startswith("widen."):
            source = operands["src"]
            shift = operands["su"]
            assert isinstance(source, tuple)
            assert isinstance(shift, int)
            assert state["saturation"] == 0
            element_bits = _integer_packet_bits(key, state)
            signed = ".signed." in key
            result = tuple(
                (
                    _signed(value, element_bits)
                    if signed
                    else value & _mask(element_bits)
                )
                << shift
                for value in source
            )
        elif key.startswith("narrow."):
            source = operands["src"]
            shift = operands["su"]
            assert isinstance(source, tuple)
            assert isinstance(shift, int)
            assert state["saturation"] == 0
            assert state["rounding"] == 0
            element_bits = _integer_packet_bits(key, state)
            wide_bits = element_bits * (4 if ".4x." in key else 2)
            signed = ".signed." in key
            result = tuple(
                (value >> shift if signed else (value & _mask(wide_bits)) >> shift)
                & _mask(element_bits)
                for value in source
            )
        else:
            raise AssertionError(f"unmodeled integer shift descriptor {key}")
        assert len(emit.results) == 1
        values[next(iter(emit.results.values()))] = result

    result = values[ValueRef.result("result")]
    assert isinstance(result, tuple)
    return result


def _pack(values: tuple[int, ...], element_bits: int) -> tuple[int, ...]:
    if element_bits != 64:
        return tuple(value & _mask(element_bits) for value in values)
    return tuple(
        word
        for value in values
        for word in (value & _mask(32), (value >> 32) & _mask(32))
    )


def _unpack(values: tuple[int, ...], element_bits: int) -> tuple[int, ...]:
    if element_bits != 64:
        return tuple(value & _mask(element_bits) for value in values)
    return tuple(
        values[index] | (values[index + 1] << 32) for index in range(0, len(values), 2)
    )


def _reference(
    operation: IntegerShiftOperation,
    value: int,
    count: int,
    element_bits: int,
) -> int:
    if operation.direction == "left":
        result = value << count
    elif operation.signedness == "signed":
        result = _signed(value, element_bits) >> count
    else:
        result = (value & _mask(element_bits)) >> count
    return result & _mask(element_bits)


def _input_values(element_bits: int) -> tuple[int, ...]:
    lane_count = 512 // element_bits
    boundaries = (
        0,
        1,
        2,
        _mask(element_bits),
        1 << (element_bits - 1),
        (1 << (element_bits - 1)) - 1,
        _mask(element_bits) // 3,
        (2 * _mask(element_bits)) // 3,
    )
    return tuple(
        (boundaries[index % len(boundaries)] ^ (index * 0x9E3779B9))
        & _mask(element_bits)
        for index in range(lane_count)
    )


def test_integer_shift_rules_cover_every_packet_type_and_mechanism() -> None:
    assert len(AIE2P_INTEGER_SHIFT_RULES) == 60
    assert {rule.report_key for rule in AIE2P_INTEGER_SHIFT_RULES} == {
        "integer_shift.uniform.accumulator",
        "integer_shift.uniform.i64_words",
        "integer_shift.varying.barrel",
    }

    for element_bits in (8, 16, 32, 64):
        shapes = [
            shape
            for shape in INTEGER_SHIFT_RULE_SHAPES
            if shape.element_bits == element_bits
        ]
        assert len(shapes) == 2
        covered = set()
        for shape in shapes:
            elements = set(
                range(shape.minimum_element_count, shape.maximum_element_count + 1)
            )
            assert covered.isdisjoint(elements)
            covered.update(elements)
            assert (
                shape.vector_type.minimum_static_elements == shape.minimum_element_count
            )
            assert (
                shape.vector_type.maximum_static_elements == shape.maximum_element_count
            )
            assert shape.vector_type.minimum_lanes is None
            assert shape.vector_type.maximum_lanes is None
        assert covered == set(range(1, 512 // element_bits + 1))

    for rule in AIE2P_INTEGER_SHIFT_RULES:
        assert all(isinstance(emit, EmitDescriptorOp) for emit in rule.emit)
        descriptor_keys = tuple(
            emit.descriptor.key
            for emit in rule.emit
            if isinstance(emit, EmitDescriptorOp)
        )
        assert not any(
            ".extract." in key or ".insert." in key for key in descriptor_keys
        )
        if rule.report_key != "integer_shift.varying.barrel":
            assert rule.priority == 1
            assert Guard.value_exact_i64("rhs") in rule.guards
        else:
            assert rule.priority == 0
            assert Guard.value_exact_i64("rhs") not in rule.guards


def test_full_and_partial_packets_share_one_bounded_recipe() -> None:
    for operation in INTEGER_SHIFT_OPERATIONS:
        for element_bits in (8, 16, 32, 64):
            shapes = [
                shape
                for shape in INTEGER_SHIFT_RULE_SHAPES
                if shape.element_bits == element_bits
            ]
            full_shape = next(
                shape
                for shape in shapes
                if shape.minimum_element_count == shape.native_element_count
            )
            partial_shape = next(shape for shape in shapes if shape is not full_shape)
            full_rules = _shape_rules(operation, full_shape)
            partial_rules = _shape_rules(operation, partial_shape)
            assert len(full_rules) == len(partial_rules)
            for full, partial in zip(full_rules, partial_rules, strict=True):
                assert full.report_key == partial.report_key
                assert [emit.descriptor.key for emit in full.emit] == [
                    emit.descriptor.key for emit in partial.emit
                ]
                assert len(full.emit) <= 128


def test_uniform_integer_shift_recipes_are_exact_for_every_valid_count() -> None:
    for operation in INTEGER_SHIFT_OPERATIONS:
        for element_bits in (8, 16, 32, 64):
            values = _input_values(element_bits)
            for count in range(element_bits):
                counts = (count,) * len(values)
                actual = _unpack(
                    _evaluate(
                        _uniform_rule(operation, element_bits, count),
                        _pack(values, element_bits),
                        _pack(counts, element_bits),
                        count,
                    ),
                    element_bits,
                )
                expected = tuple(
                    _reference(operation, value, count, element_bits)
                    for value in values
                )
                assert actual == expected


def test_varying_integer_shift_recipes_cover_every_valid_count() -> None:
    for operation in INTEGER_SHIFT_OPERATIONS:
        for element_bits in (8, 16, 32, 64):
            values = _input_values(element_bits)
            lane_count = len(values)
            rule = _varying_rule(operation, element_bits)
            for first_count in range(0, element_bits, lane_count):
                counts = tuple(
                    (first_count + lane) % element_bits for lane in range(lane_count)
                )
                actual = _unpack(
                    _evaluate(
                        rule,
                        _pack(values, element_bits),
                        _pack(counts, element_bits),
                        0,
                    ),
                    element_bits,
                )
                expected = tuple(
                    _reference(operation, value, count, element_bits)
                    for value, count in zip(values, counts, strict=True)
                )
                assert actual == expected
