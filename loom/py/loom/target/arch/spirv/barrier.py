# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source-of-truth rows for SPIR-V control barriers."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class ControlBarrierOrdering:
    source_keyword: str
    memory_semantics: str


@dataclass(frozen=True, slots=True)
class ControlBarrierScope:
    source_keyword: str
    execution_scope: str
    memory_scope: str


@dataclass(frozen=True, slots=True)
class ControlBarrierMemorySpace:
    source_keyword: str
    memory_semantics: str
    orderings: tuple[ControlBarrierOrdering, ...]


@dataclass(frozen=True, slots=True)
class ControlBarrierCase:
    memory_space: ControlBarrierMemorySpace
    ordering: ControlBarrierOrdering
    scope: ControlBarrierScope

    @property
    def descriptor_key(self) -> str:
        return (
            f"spirv.op_control_barrier.{self.scope.source_keyword}."
            f"{self.memory_space.source_keyword}.{self.ordering.source_keyword}"
        )

    @property
    def mnemonic(self) -> str:
        suffix = self.descriptor_key.removeprefix("spirv.op_control_barrier.")
        return f"OpControlBarrier.{suffix}"

    @property
    def memory_semantics(self) -> str:
        return (
            f"{self.ordering.memory_semantics} | {self.memory_space.memory_semantics}"
        )


CONTROL_BARRIER_ORDERINGS = (
    ControlBarrierOrdering(
        source_keyword="acquire",
        memory_semantics="LOOM_SPIRV_MEMORY_SEMANTICS_ACQUIRE_MASK",
    ),
    ControlBarrierOrdering(
        source_keyword="release",
        memory_semantics="LOOM_SPIRV_MEMORY_SEMANTICS_RELEASE_MASK",
    ),
    ControlBarrierOrdering(
        source_keyword="acq_rel",
        memory_semantics="LOOM_SPIRV_MEMORY_SEMANTICS_ACQUIRE_RELEASE_MASK",
    ),
)

CONTROL_BARRIER_SCOPES = (
    ControlBarrierScope(
        source_keyword="subgroup",
        execution_scope="LOOM_SPIRV_SCOPE_SUBGROUP",
        memory_scope="LOOM_SPIRV_SCOPE_WORKGROUP",
    ),
    ControlBarrierScope(
        source_keyword="workgroup",
        execution_scope="LOOM_SPIRV_SCOPE_WORKGROUP",
        memory_scope="LOOM_SPIRV_SCOPE_WORKGROUP",
    ),
)

CONTROL_BARRIER_MEMORY_SPACES = (
    ControlBarrierMemorySpace(
        source_keyword="workgroup",
        memory_semantics="LOOM_SPIRV_MEMORY_SEMANTICS_WORKGROUP_MEMORY_MASK",
        orderings=(CONTROL_BARRIER_ORDERINGS[2],),
    ),
    ControlBarrierMemorySpace(
        source_keyword="global",
        memory_semantics="LOOM_SPIRV_MEMORY_SEMANTICS_UNIFORM_MEMORY_MASK",
        orderings=CONTROL_BARRIER_ORDERINGS,
    ),
)

CONTROL_BARRIER_CASES = tuple(
    ControlBarrierCase(memory_space, ordering, scope)
    for memory_space in CONTROL_BARRIER_MEMORY_SPACES
    for ordering in memory_space.orderings
    for scope in CONTROL_BARRIER_SCOPES
)
