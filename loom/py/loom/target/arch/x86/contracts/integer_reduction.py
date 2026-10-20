# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 full-register integer reduction families."""

from __future__ import annotations

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import (
    emit_descriptor_op as _op_emit,
)
from loom.target.arch.x86.contracts.rule_builders import (
    zmm_reduction_emit_chain as _zmm_reduction_emit_chain,
)
from loom.target.arch.x86.vector_families import (
    AVX2_INTEGER_REDUCTION_FAMILIES,
    AVX2_VECTOR_BIT_WIDTHS,
    AVX512_INTEGER_REDUCTION_FAMILIES,
    AVX512_VECTOR_BIT_WIDTHS,
    X86_LANE_FAMILIES,
    VectorBinaryFamily,
)
from loom.target.contracts import (
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
    Vector,
)
from loom.target.low_descriptors import Descriptor

_BROADCAST_MNEMONICS = {
    8: "vpbroadcastb",
    16: "vpbroadcastw",
    32: "vpbroadcastd",
    64: "vpbroadcastq",
}
_EXTRACT_MNEMONICS = {
    row.element_bit_width: row.extract_mnemonic for row in X86_LANE_FAMILIES
}


def _integer_reduction_rule(
    row: VectorBinaryFamily,
    vector_bit_width: int,
    descriptor_prefix: str,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    element_bit_width = row.element.bit_width
    vector_type = Vector(
        row.element.name,
        lanes=vector_bit_width // element_bit_width,
    )
    scalar_type = Scalar(row.element.name)
    combine_register_suffix = "zmm" if vector_bit_width == 512 else "xmm"
    combine = descriptor_lookup(
        f"{descriptor_prefix}.{row.mnemonic}.{combine_register_suffix}"
    )
    emits: list[EmitDescriptorOp] = []
    dependencies: list[Descriptor] = []
    reduced = ValueRef.operand("input")
    if vector_bit_width == 256:
        extract_half = descriptor_lookup("x86.avx2.vextractf128.xmm.ymm")
        dependencies.append(extract_half)
        low_half = ValueRef.temporary("low_half")
        high_half = ValueRef.temporary("high_half")
        half_sum = ValueRef.temporary("half_sum")
        emits.extend(
            (
                _op_emit(
                    descriptor=extract_half,
                    operands={"source": reduced},
                    results={"dst": low_half},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"lane": 0},
                ),
                _op_emit(
                    descriptor=extract_half,
                    operands={"source": reduced},
                    results={"dst": high_half},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"lane": 1},
                ),
                _op_emit(
                    descriptor=combine,
                    operands={"lhs": low_half, "rhs": high_half},
                    results={"dst": half_sum},
                    result_types={"dst": DescriptorResultType()},
                ),
            )
        )
        reduced = half_sum
    elif vector_bit_width == 512:
        reduction_emits, reduced, reduction_dependencies = _zmm_reduction_emit_chain(
            reduced,
            combine,
            element_bit_width,
            descriptor_lookup,
        )
        emits.extend(reduction_emits)
        dependencies.extend(reduction_dependencies)

    if vector_bit_width != 512:
        shift = descriptor_lookup("x86.avx2.vpsrldq.xmm")
        dependencies.append(shift)
        shift_bytes = 8
        reduction_ordinal = 0
        while shift_bytes >= element_bit_width // 8:
            shifted = ValueRef.temporary(f"shifted{reduction_ordinal}")
            next_reduced = ValueRef.temporary(f"reduced{reduction_ordinal}")
            emits.extend(
                (
                    _op_emit(
                        descriptor=shift,
                        operands={"source": reduced},
                        results={"dst": shifted},
                        result_types={"dst": DescriptorResultType()},
                        immediates={"bytes": shift_bytes},
                    ),
                    _op_emit(
                        descriptor=combine,
                        operands={"lhs": reduced, "rhs": shifted},
                        results={"dst": next_reduced},
                        result_types={"dst": DescriptorResultType()},
                    ),
                )
            )
            reduced = next_reduced
            reduction_ordinal += 1
            shift_bytes //= 2

    if vector_bit_width == 512:
        move_init = descriptor_lookup(
            f"x86.avx512.{_BROADCAST_MNEMONICS[element_bit_width]}.zmm"
        )
        init_operand_name = "value"
    else:
        move_init = descriptor_lookup(
            "x86.avx2.vmovq.xmm.gpr64"
            if element_bit_width == 64
            else "x86.avx2.vmovd.xmm.gpr32"
        )
        init_operand_name = "input"
    dependencies.append(move_init)
    init_vector = ValueRef.temporary("init_vector")
    with_init = ValueRef.temporary("with_init")
    emits.extend(
        (
            _op_emit(
                descriptor=move_init,
                operands={init_operand_name: ValueRef.operand("init")},
                results={"dst": init_vector},
                result_types={"dst": DescriptorResultType()},
            ),
            _op_emit(
                descriptor=combine,
                operands={"lhs": reduced, "rhs": init_vector},
                results={"dst": with_init},
                result_types={"dst": DescriptorResultType()},
            ),
        )
    )

    if vector_bit_width == 512:
        extract_quarter = descriptor_lookup("x86.avx512.vextractf32x4.xmm.zmm")
        dependencies.append(extract_quarter)
        extracted = ValueRef.temporary("extracted")
        emits.append(
            _op_emit(
                descriptor=extract_quarter,
                operands={"source": with_init},
                results={"dst": extracted},
                result_types={"dst": DescriptorResultType()},
                immediates={"lane": 0},
            )
        )
        with_init = extracted

    extract_mnemonic = _EXTRACT_MNEMONICS[element_bit_width]
    extract = descriptor_lookup(
        f"x86.avx2.{extract_mnemonic}.gpr{max(32, element_bit_width)}.xmm"
    )
    dependencies.append(extract)
    emits.append(
        _op_emit(
            descriptor=extract,
            operands={"source": with_init},
            results={"dst": ValueRef.result("result")},
            immediates={"lane": 0},
        )
    )
    return DescriptorRule(
        source_op=vector.vector_reduce,
        descriptor=combine,
        guards=(
            Guard.enum_attr_equals("kind", row.source_operation),
            Guard.value_type("input", vector_type),
            Guard.value_type("init", scalar_type),
            Guard.value_type("result", scalar_type),
            *(
                Guard.descriptor_available(descriptor)
                for descriptor in dict.fromkeys(dependencies)
            ),
        ),
        emit=tuple(emits),
    )


def avx2_integer_reduction_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Generates every natively composable AVX2 integer reduction."""
    return tuple(
        _integer_reduction_rule(
            row,
            vector_bit_width,
            "x86.avx2",
            descriptor_lookup,
        )
        for row in AVX2_INTEGER_REDUCTION_FAMILIES
        for vector_bit_width in AVX2_VECTOR_BIT_WIDTHS
    )


def avx512_integer_reduction_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Generates every natively composable AVX-512 integer reduction."""
    return tuple(
        _integer_reduction_rule(
            row,
            vector_bit_width,
            "x86.avx512",
            descriptor_lookup,
        )
        for row in AVX512_INTEGER_REDUCTION_FAMILIES
        for vector_bit_width in AVX512_VECTOR_BIT_WIDTHS
    )
