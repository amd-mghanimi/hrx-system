# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.kernel import KernelOrdering, KernelScope
from loom.target.arch.spirv.barrier import (
    CONTROL_BARRIER_CASES,
    CONTROL_BARRIER_MEMORY_SPACES,
    CONTROL_BARRIER_ORDERINGS,
    CONTROL_BARRIER_SCOPES,
)


def test_control_barrier_matrix_matches_the_source_contract() -> None:
    assert tuple(scope.source_keyword for scope in CONTROL_BARRIER_SCOPES) == tuple(
        case.keyword for case in KernelScope.cases
    )
    assert tuple(
        ordering.source_keyword for ordering in CONTROL_BARRIER_ORDERINGS
    ) == tuple(
        case.keyword
        for case in KernelOrdering.cases
        if case.keyword in ("acquire", "release", "acq_rel")
    )
    assert tuple(
        memory_space.source_keyword for memory_space in CONTROL_BARRIER_MEMORY_SPACES
    ) == ("workgroup", "global")

    cases = {
        (
            case.memory_space.source_keyword,
            case.ordering.source_keyword,
            case.scope.source_keyword,
        )
        for case in CONTROL_BARRIER_CASES
    }
    assert cases == {
        ("workgroup", "acq_rel", "subgroup"),
        ("workgroup", "acq_rel", "workgroup"),
        ("global", "acquire", "subgroup"),
        ("global", "acquire", "workgroup"),
        ("global", "release", "subgroup"),
        ("global", "release", "workgroup"),
        ("global", "acq_rel", "subgroup"),
        ("global", "acq_rel", "workgroup"),
    }


def test_control_barrier_descriptor_names_are_unique() -> None:
    descriptor_keys = tuple(case.descriptor_key for case in CONTROL_BARRIER_CASES)
    mnemonics = tuple(case.mnemonic for case in CONTROL_BARRIER_CASES)
    assert len(set(descriptor_keys)) == len(descriptor_keys)
    assert len(set(mnemonics)) == len(mnemonics)
