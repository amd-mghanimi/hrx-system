# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 vector shuffle contract families."""

from __future__ import annotations

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import (
    emit_descriptor_op as _op_emit,
)
from loom.target.arch.x86.vector_families import AVX2_SHUFFLE_FAMILIES
from loom.target.contracts import (
    AttrProject,
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    TypePattern,
    ValueRef,
    Vector,
)
from loom.target.low_descriptors import Descriptor


def _immediate_shuffle_rule(
    type_pattern: TypePattern,
    descriptor_key: str,
    lane_count: int,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    if lane_count not in (2, 4):
        raise ValueError("immediate x86 shuffle requires two or four lanes")
    descriptor = descriptor_lookup(descriptor_key)
    return DescriptorRule(
        source_op=vector.vector_shuffle,
        descriptor=descriptor,
        guards=(
            Guard.value_type("source", type_pattern),
            Guard.value_type("result", type_pattern),
            Guard.i64_array_count("source_lanes", lane_count),
            Guard.i64_array_elements_range("source_lanes", 0, lane_count - 1),
        ),
        emit=(
            _op_emit(
                descriptor=descriptor,
                operands={"source": ValueRef.operand("source")},
                results={"dst": ValueRef.result("result")},
                immediates={
                    "control": AttrProject.i64_array_pack_elements(
                        "source_lanes",
                        element=0,
                        count=lane_count,
                        bit_width=lane_count.bit_length() - 1,
                    )
                },
            ),
        ),
        priority=2,
    )


def _control_load(
    descriptor: Descriptor,
    result: ValueRef,
    projection: AttrProject,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=descriptor,
        results={"dst": result},
        result_types={"dst": DescriptorResultType()},
        immediates={"data": projection},
        form=DescriptorEmitForm.OP,
    )


def _read_only_shuffle_rule(
    *,
    type_pattern: TypePattern,
    lane_count: int,
    load: Descriptor,
    shuffle: Descriptor,
    projection: AttrProject,
    shuffle_operands: dict[str, ValueRef],
    priority: int = 1,
) -> DescriptorRule:
    control = ValueRef.temporary("control")
    return DescriptorRule(
        source_op=vector.vector_shuffle,
        descriptor=shuffle,
        guards=(
            Guard.value_type("source", type_pattern),
            Guard.value_type("result", type_pattern),
            Guard.i64_array_count("source_lanes", lane_count),
            Guard.i64_array_elements_range("source_lanes", 0, lane_count - 1),
            Guard.descriptor_available(load),
        ),
        emit=(
            _control_load(load, control, projection),
            _op_emit(
                descriptor=shuffle,
                operands=shuffle_operands,
                results={"dst": ValueRef.result("result")},
            ),
        ),
        priority=priority,
    )


def _xmm_byte_shuffle_rule(
    *,
    element_names: tuple[str, ...],
    bytes_per_lane: int,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    lane_count = 16 // bytes_per_lane
    return _read_only_shuffle_rule(
        type_pattern=Vector(element_names, lanes=lane_count),
        lane_count=lane_count,
        load=descriptor_lookup("x86.avx2.vmovdqu.rodata.xmm"),
        shuffle=descriptor_lookup("x86.avx2.vpshufb.xmm"),
        projection=AttrProject.i64_array_read_only_byte_segment(
            "source_lanes",
            bytes_per_lane=bytes_per_lane,
            source_byte_offset=0,
        ),
        shuffle_operands={
            "lhs": ValueRef.operand("source"),
            "rhs": ValueRef.temporary("control"),
        },
    )


def _ymm_dword_shuffle_rule(
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    return _read_only_shuffle_rule(
        type_pattern=Vector(("i32", "f32"), lanes=8),
        lane_count=8,
        load=descriptor_lookup("x86.avx2.vmovdqu.rodata.ymm"),
        shuffle=descriptor_lookup("x86.avx2.vpermps.ymm"),
        projection=AttrProject.i64_array_read_only_elements(
            "source_lanes", bit_width=32
        ),
        shuffle_operands={
            "control": ValueRef.temporary("control"),
            "source": ValueRef.operand("source"),
        },
        priority=2,
    )


def _ymm_byte_shuffle_rule(
    *,
    element_names: tuple[str, ...],
    bytes_per_lane: int,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    lane_count = 32 // bytes_per_lane
    type_pattern = Vector(element_names, lanes=lane_count)
    load = descriptor_lookup("x86.avx2.vmovdqu.rodata.ymm")
    duplicate = descriptor_lookup("x86.avx2.vpermq.ymm")
    shuffle = descriptor_lookup("x86.avx2.vpshufb.ymm")
    combine = descriptor_lookup("x86.avx2.vpor.ymm")
    emits: list[EmitDescriptorOp] = []
    shuffled_sources: list[ValueRef] = []
    for source_half in range(2):
        control = ValueRef.temporary(f"control{source_half}")
        duplicated_source = ValueRef.temporary(f"source{source_half}")
        shuffled_source = ValueRef.temporary(f"shuffled{source_half}")
        emits.extend(
            (
                _control_load(
                    load,
                    control,
                    AttrProject.i64_array_read_only_byte_segment(
                        "source_lanes",
                        bytes_per_lane=bytes_per_lane,
                        source_byte_offset=source_half * 16,
                    ),
                ),
                _op_emit(
                    descriptor=duplicate,
                    operands={"source": ValueRef.operand("source")},
                    results={"dst": duplicated_source},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"control": 0x44 if source_half == 0 else 0xEE},
                ),
                _op_emit(
                    descriptor=shuffle,
                    operands={"lhs": duplicated_source, "rhs": control},
                    results={"dst": shuffled_source},
                    result_types={"dst": DescriptorResultType()},
                ),
            )
        )
        shuffled_sources.append(shuffled_source)
    emits.append(
        _op_emit(
            descriptor=combine,
            operands={"lhs": shuffled_sources[0], "rhs": shuffled_sources[1]},
            results={"dst": ValueRef.result("result")},
        )
    )
    return DescriptorRule(
        source_op=vector.vector_shuffle,
        descriptor=shuffle,
        guards=(
            Guard.value_type("source", type_pattern),
            Guard.value_type("result", type_pattern),
            Guard.i64_array_count("source_lanes", lane_count),
            Guard.i64_array_elements_range("source_lanes", 0, lane_count - 1),
            *(Guard.descriptor_available(row) for row in (load, duplicate, combine)),
        ),
        emit=tuple(emits),
        priority=1,
    )


def avx2_shuffle_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    byte_lane_rules = tuple(
        rule
        for family in AVX2_SHUFFLE_FAMILIES
        if family.element_bit_width in (8, 16)
        for rule in (
            _xmm_byte_shuffle_rule(
                element_names=family.element_names,
                bytes_per_lane=family.element_bit_width // 8,
                descriptor_lookup=descriptor_lookup,
            ),
            _ymm_byte_shuffle_rule(
                element_names=family.element_names,
                bytes_per_lane=family.element_bit_width // 8,
                descriptor_lookup=descriptor_lookup,
            ),
        )
    )
    return (
        _immediate_shuffle_rule(
            Vector(("i32", "f32"), lanes=4),
            "x86.avx2.vpshufd.xmm",
            4,
            descriptor_lookup,
        ),
        _immediate_shuffle_rule(
            Vector(("i64", "f64"), lanes=2),
            "x86.avx2.vpermilpd.xmm",
            2,
            descriptor_lookup,
        ),
        _immediate_shuffle_rule(
            Vector(("i64", "f64"), lanes=4),
            "x86.avx2.vpermq.ymm",
            4,
            descriptor_lookup,
        ),
        _ymm_dword_shuffle_rule(descriptor_lookup),
        *byte_lane_rules,
    )


def x86_low_xmm_word_shuffle_rule(
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    """Builds the four-word shuffle carried in the low half of XMM."""
    return _immediate_shuffle_rule(
        Vector(("i16", "f16"), lanes=4),
        "x86.avx2.vpshuflw.xmm",
        4,
        descriptor_lookup,
    )


def _zmm_direct_shuffle_rule(
    *,
    element_names: tuple[str, ...],
    element_bit_width: int,
    mnemonic: str,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    lane_count = 512 // element_bit_width
    return _read_only_shuffle_rule(
        type_pattern=Vector(element_names, lanes=lane_count),
        lane_count=lane_count,
        load=descriptor_lookup("x86.avx512.vmovdqu64.rodata.zmm"),
        shuffle=descriptor_lookup(f"x86.avx512.{mnemonic}.zmm"),
        projection=AttrProject.i64_array_read_only_elements(
            "source_lanes", bit_width=element_bit_width
        ),
        shuffle_operands={
            "source": ValueRef.operand("source"),
            "control": ValueRef.temporary("control"),
        },
        priority=2,
    )


def _zmm_byte_shuffle_rule(
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    type_pattern = Vector(("i8", "f8E4M3", "f8E5M2"), lanes=64)
    load = descriptor_lookup("x86.avx512.vmovdqu64.rodata.zmm")
    permute = descriptor_lookup("x86.avx512.vpermw.zmm")
    shift_right_immediate = descriptor_lookup("x86.avx512.vpsrlw.zmm")
    shift_right_variable = descriptor_lookup("x86.avx512.vpsrlvw.zmm")
    shift_left_immediate = descriptor_lookup("x86.avx512.vpsllw.zmm")
    combine = descriptor_lookup("x86.avx512.vpord.zmm")
    emits: list[EmitDescriptorOp] = []
    shifted_bytes: list[ValueRef] = []
    for byte_parity in range(2):
        control = ValueRef.temporary(f"control{byte_parity}")
        selected_words = ValueRef.temporary(f"words{byte_parity}")
        counts = ValueRef.temporary(f"counts{byte_parity}")
        selected_bytes = ValueRef.temporary(f"bytes{byte_parity}")
        emits.extend(
            (
                _control_load(
                    load,
                    control,
                    AttrProject.i64_array_read_only_byte_words(
                        "source_lanes", byte_parity=byte_parity
                    ),
                ),
                _op_emit(
                    descriptor=permute,
                    operands={
                        "source": ValueRef.operand("source"),
                        "control": control,
                    },
                    results={"dst": selected_words},
                    result_types={"dst": DescriptorResultType()},
                ),
                _op_emit(
                    descriptor=shift_right_immediate,
                    operands={"source": control},
                    results={"dst": counts},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"shift": 5},
                ),
                _op_emit(
                    descriptor=shift_right_variable,
                    operands={"lhs": selected_words, "rhs": counts},
                    results={"dst": selected_bytes},
                    result_types={"dst": DescriptorResultType()},
                ),
            )
        )
        shifted_bytes.append(selected_bytes)

    even_high = ValueRef.temporary("even_high")
    even_low = ValueRef.temporary("even_low")
    odd_high = ValueRef.temporary("odd_high")
    emits.extend(
        (
            _op_emit(
                descriptor=shift_left_immediate,
                operands={"source": shifted_bytes[0]},
                results={"dst": even_high},
                result_types={"dst": DescriptorResultType()},
                immediates={"shift": 8},
            ),
            _op_emit(
                descriptor=shift_right_immediate,
                operands={"source": even_high},
                results={"dst": even_low},
                result_types={"dst": DescriptorResultType()},
                immediates={"shift": 8},
            ),
            _op_emit(
                descriptor=shift_left_immediate,
                operands={"source": shifted_bytes[1]},
                results={"dst": odd_high},
                result_types={"dst": DescriptorResultType()},
                immediates={"shift": 8},
            ),
            _op_emit(
                descriptor=combine,
                operands={"lhs": even_low, "rhs": odd_high},
                results={"dst": ValueRef.result("result")},
            ),
        )
    )
    return DescriptorRule(
        source_op=vector.vector_shuffle,
        descriptor=permute,
        guards=(
            Guard.value_type("source", type_pattern),
            Guard.value_type("result", type_pattern),
            Guard.i64_array_count("source_lanes", 64),
            Guard.i64_array_elements_range("source_lanes", 0, 63),
            *(
                Guard.descriptor_available(row)
                for row in (
                    load,
                    shift_right_immediate,
                    shift_right_variable,
                    shift_left_immediate,
                    combine,
                )
            ),
        ),
        emit=tuple(emits),
        priority=1,
    )


def avx512_shuffle_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    return (
        _zmm_byte_shuffle_rule(descriptor_lookup),
        _zmm_direct_shuffle_rule(
            element_names=("i16", "f16", "bf16"),
            element_bit_width=16,
            mnemonic="vpermw",
            descriptor_lookup=descriptor_lookup,
        ),
        _zmm_direct_shuffle_rule(
            element_names=("i32", "f32"),
            element_bit_width=32,
            mnemonic="vpermd",
            descriptor_lookup=descriptor_lookup,
        ),
        _zmm_direct_shuffle_rule(
            element_names=("i64", "f64"),
            element_bit_width=64,
            mnemonic="vpermq",
            descriptor_lookup=descriptor_lookup,
        ),
    )
