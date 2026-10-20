# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Target-owned source value materializer callbacks."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class ValueMaterializer:
    """Selection, carrier, and emission contracts for a materialized value.

    A prepare callback retains source-dependent choices in the function plan.
    Its materialize callback consumes that recipe after source analysis ends.
    Without prepare, materialize consumes only the emitted Low carrier.
    """

    name: str
    can_materialize: str
    result_type: str
    materialize: str
    header: str
    prepare: str | None = None

    def __post_init__(self) -> None:
        if not self.name:
            raise ValueError("value materializer name must be non-empty")
        if not self.can_materialize:
            raise ValueError(f"value materializer '{self.name}' needs a predicate")
        if not self.materialize:
            raise ValueError(f"value materializer '{self.name}' needs an emitter")
        if not self.result_type:
            raise ValueError(f"value materializer '{self.name}' needs a native type")
        if not self.header:
            raise ValueError(f"value materializer '{self.name}' needs a C header")
