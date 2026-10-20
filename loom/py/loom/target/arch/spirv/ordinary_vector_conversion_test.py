# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from collections import Counter

from loom.target.arch.spirv.ordinary_vector import (
    NATIVE_ORDINARY_VECTOR_LANE_COUNTS,
    ORDINARY_VECTOR_INSTRUCTIONS,
    OrdinaryVectorComponentKind,
)
from loom.target.arch.spirv.ordinary_vector_conversion import (
    NATIVE_VECTOR_SCALAR_CONVERSIONS,
    ORDINARY_VECTOR_CONVERSION_INSTRUCTIONS,
    ORDINARY_VECTOR_CONVERSIONS,
)
from loom.target.arch.spirv.ordinary_vector_integer import (
    ORDINARY_VECTOR_INTEGER_INSTRUCTIONS,
)
from loom.target.arch.spirv.scalar_conversion import (
    SIGNED_INTEGER_WIDTH_CONVERSIONS,
    UNSIGNED_INTEGER_WIDTH_CONVERSIONS,
)


def test_numeric_conversion_matrix_is_the_exact_scalar_lane_cross_product() -> None:
    assert len(SIGNED_INTEGER_WIDTH_CONVERSIONS) == 12
    assert len(UNSIGNED_INTEGER_WIDTH_CONVERSIONS) == 6
    assert len(NATIVE_VECTOR_SCALAR_CONVERSIONS) == 72
    assert len(ORDINARY_VECTOR_CONVERSIONS) == 216

    assert {
        (conversion.scalar_conversion, conversion.source_type.lane_count)
        for conversion in ORDINARY_VECTOR_CONVERSIONS
    } == {
        (scalar_conversion, lane_count)
        for scalar_conversion in NATIVE_VECTOR_SCALAR_CONVERSIONS
        for lane_count in NATIVE_ORDINARY_VECTOR_LANE_COUNTS
    }

    expected_pairs = set()
    integer_types = ("i8", "i16", "i32", "i64")
    float_types = ("f16", "f32", "f64")
    for source_width, source in enumerate(integer_types):
        for result_width, result in enumerate(integer_types):
            if source_width < result_width:
                expected_pairs.add(("extsi", source, result))
                expected_pairs.add(("extui", source, result))
            elif source_width > result_width:
                expected_pairs.add(("trunci", source, result))
        for result in float_types:
            expected_pairs.add(("sitofp", source, result))
            expected_pairs.add(("uitofp", source, result))
            expected_pairs.add(("fptosi", result, source))
            expected_pairs.add(("fptoui", result, source))
    for source_width, source in enumerate(float_types):
        for result_width, result in enumerate(float_types):
            if source_width != result_width:
                operation = "extf" if source_width < result_width else "fptrunc"
                expected_pairs.add((operation, source, result))
    assert {
        (row.source_op_key, row.source_type.source_type, row.result_type.source_type)
        for row in NATIVE_VECTOR_SCALAR_CONVERSIONS
    } == expected_pairs


def test_integer_conversion_rows_preserve_semantics_and_native_types() -> None:
    signed_conversions = tuple(
        conversion
        for conversion in ORDINARY_VECTOR_CONVERSIONS
        if conversion.scalar_conversion in SIGNED_INTEGER_WIDTH_CONVERSIONS
    )
    unsigned_conversions = tuple(
        conversion
        for conversion in ORDINARY_VECTOR_CONVERSIONS
        if conversion.scalar_conversion in UNSIGNED_INTEGER_WIDTH_CONVERSIONS
    )
    assert len(signed_conversions) == 36
    assert len(unsigned_conversions) == 18
    for conversion in signed_conversions:
        assert conversion.source_type.component_type.kind == (
            OrdinaryVectorComponentKind.SIGNED_INTEGER
        )
        assert conversion.result_type.component_type.kind == (
            OrdinaryVectorComponentKind.SIGNED_INTEGER
        )
        source_width = conversion.source_type.component_type.bit_width
        result_width = conversion.result_type.component_type.bit_width
        if conversion.scalar_conversion.source_op_key == "extsi":
            assert source_width < result_width
        else:
            assert conversion.scalar_conversion.source_op_key == "trunci"
            assert source_width > result_width

    for conversion in unsigned_conversions:
        assert conversion.scalar_conversion.source_op_key == "extui"
        assert conversion.source_type.component_type.kind == (
            OrdinaryVectorComponentKind.UNSIGNED_INTEGER
        )
        assert conversion.result_type.component_type.kind == (
            OrdinaryVectorComponentKind.UNSIGNED_INTEGER
        )
        assert (
            conversion.source_type.component_type.bit_width
            < conversion.result_type.component_type.bit_width
        )

    for conversion in ORDINARY_VECTOR_CONVERSIONS:
        scalar_conversion = conversion.scalar_conversion
        assert conversion.source_type.lane_count == conversion.result_type.lane_count
        assert conversion.source_type.component_type.suffix == (
            scalar_conversion.source_type.suffix
        )
        assert conversion.result_type.component_type.suffix == (
            scalar_conversion.result_type.suffix
        )
        assert conversion.instruction.feature_bits == scalar_conversion.feature_bits


def test_numeric_conversion_instructions_are_exact_and_unique() -> None:
    assert len(ORDINARY_VECTOR_CONVERSION_INSTRUCTIONS) == 216
    instruction_keys = tuple(
        instruction.key for instruction in ORDINARY_VECTOR_CONVERSION_INSTRUCTIONS
    )
    assert len(set(instruction_keys)) == len(instruction_keys)
    assert set(instruction_keys).isdisjoint(
        instruction.key
        for instruction in (
            *ORDINARY_VECTOR_INSTRUCTIONS,
            *ORDINARY_VECTOR_INTEGER_INSTRUCTIONS,
        )
    )
    assert Counter(
        instruction.opcode for instruction in ORDINARY_VECTOR_CONVERSION_INSTRUCTIONS
    ) == {
        "LOOM_SPIRV_OP_S_CONVERT": 36,
        "LOOM_SPIRV_OP_U_CONVERT": 18,
        "LOOM_SPIRV_OP_F_CONVERT": 18,
        "LOOM_SPIRV_OP_CONVERT_S_TO_F": 36,
        "LOOM_SPIRV_OP_CONVERT_U_TO_F": 36,
        "LOOM_SPIRV_OP_CONVERT_F_TO_S": 36,
        "LOOM_SPIRV_OP_CONVERT_F_TO_U": 36,
    }

    for conversion, instruction in zip(
        ORDINARY_VECTOR_CONVERSIONS,
        ORDINARY_VECTOR_CONVERSION_INSTRUCTIONS,
        strict=True,
    ):
        assert instruction == conversion.instruction
        assert instruction.packet_form == "LOOM_SPIRV_PACKET_FORM_UNARY_TYPED"
        assert instruction.key == (
            f"spirv.op_{conversion.scalar_conversion.descriptor_suffix}."
            f"{conversion.source_type.suffix}.{conversion.result_type.suffix}"
        )
        assert instruction.mnemonic == (
            f"{conversion.scalar_conversion.mnemonic}."
            f"{conversion.source_type.suffix}.{conversion.result_type.suffix}"
        )
        assert instruction.opcode == conversion.scalar_conversion.opcode
        assert instruction.result_type == conversion.result_type
        assert instruction.operand_names == ("input",)
        assert instruction.operand_types == (conversion.source_type,)
