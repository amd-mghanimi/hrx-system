# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Audits native AIE2P interleave selection across the carrier family."""

import argparse
import re
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path


def _uses_block_route(chunk_byte_count: int) -> bool:
    return chunk_byte_count > 32 or (chunk_byte_count & (chunk_byte_count - 1)) != 0


@dataclass(frozen=True)
class InterleaveCase:
    name: str
    operation: str
    half_type: str
    combined_type: str
    axis: int
    predicate: bool
    chunk_byte_count: int

    @property
    def plan_key(self) -> str:
        predicate_prefix = "predicate-" if self.predicate else ""
        mechanism = "block-route" if self.uses_block_route else "vshuffle"
        return f"{self.operation}.{predicate_prefix}{mechanism}"

    @property
    def uses_block_route(self) -> bool:
        return _uses_block_route(self.chunk_byte_count)

    def source(self) -> str:
        if self.operation == "interleave":
            signature = (
                f"func.def target(@target) @{self.name}(%even: {self.half_type}, "
                f"%odd: {self.half_type}) -> ({self.combined_type}) {{"
            )
            interleave = (
                f"  %result = vector.interleave<{self.axis}> %even, %odd : "
                f"{self.half_type}, {self.half_type} -> {self.combined_type}"
            )
            return "\n".join(
                (
                    signature,
                    interleave,
                    f"  func.return %result : {self.combined_type}",
                    "}",
                )
            )
        signature = (
            f"func.def target(@target) @{self.name}(%source: {self.combined_type}) "
            f"-> ({self.half_type}, {self.half_type}) {{"
        )
        deinterleave = (
            f"  %even, %odd = vector.deinterleave<{self.axis}> %source : "
            f"{self.combined_type} -> {self.half_type}, {self.half_type}"
        )
        return "\n".join(
            (
                signature,
                deinterleave,
                f"  func.return %even, %odd : {self.half_type}, {self.half_type}",
                "}",
            )
        )

    def check(self) -> str:
        return (
            "CHECK: COMPILE-REPORT: source_low[*] "
            f"function={self.name} source_op=vector.{self.operation} "
            f"selection=plan plan_key={self.plan_key} *"
        )


_ELEMENT_FAMILIES = (
    ("i1", 64, 64, True),
    ("i8", 64, 64, False),
    ("f8E4M3", 64, 64, False),
    ("f8E5M2", 64, 64, False),
    ("i16", 32, 32, False),
    ("f16", 32, 32, False),
    ("bf16", 32, 32, False),
    ("i32", 16, 32, False),
    ("f32", 16, 32, False),
    ("index", 16, 32, False),
    ("offset", 16, 32, False),
    ("i64", 8, 16, False),
    ("f64", 8, 8, False),
)

_MAX_PRESSURE_CODE_BYTES = 1024


def _vector_type(dimensions: tuple[int, ...], element_type: str) -> str:
    return (
        f"vector<{'x'.join(str(dimension) for dimension in dimensions)}x{element_type}>"
    )


def _rank_one_cases(exhaustive: bool) -> list[InterleaveCase]:
    cases = []
    for (
        element_type,
        packet_lanes,
        maximum_half_lanes,
        predicate,
    ) in _ELEMENT_FAMILIES:
        if exhaustive:
            lane_counts = range(1, maximum_half_lanes + 1)
        else:
            lane_counts = sorted(
                {
                    1,
                    packet_lanes // 2,
                    packet_lanes // 2 + 1,
                    packet_lanes,
                    min(packet_lanes + 1, maximum_half_lanes),
                    maximum_half_lanes,
                }
            )
        for half_lanes in lane_counts:
            half_type = _vector_type((half_lanes,), element_type)
            combined_type = _vector_type((half_lanes * 2,), element_type)
            type_name = element_type.lower()
            for operation in ("interleave", "deinterleave"):
                cases.append(
                    InterleaveCase(
                        name=f"audit_{operation}_{type_name}_{half_lanes}",
                        operation=operation,
                        half_type=half_type,
                        combined_type=combined_type,
                        axis=0,
                        predicate=predicate,
                        chunk_byte_count=64 // packet_lanes,
                    )
                )
    return cases


ShapedLayout = tuple[str, int, tuple[int, ...], int, bool]


def _representative_shaped_layouts() -> list[ShapedLayout]:
    layouts = []
    for trailing_lanes in (1, 2, 4, 8, 16, 32):
        layouts.append(("i8", 64, (2, trailing_lanes), 0, False))
    for trailing_lanes in (1, 2, 4, 8, 16):
        layouts.append(("i8", 64, (2, 2, trailing_lanes), 1, False))
    for trailing_lanes in (1, 2, 4, 8):
        layouts.append(("i8", 64, (2, 2, trailing_lanes), 0, False))
    for trailing_lanes in (1, 32):
        layouts.append(("i1", 64, (2, trailing_lanes), 0, True))

    layouts.extend(
        (
            (element_type, packet_lanes, (2, 3), 0, predicate)
            for (
                element_type,
                packet_lanes,
                _maximum_half_lanes,
                predicate,
            ) in _ELEMENT_FAMILIES
        )
    )
    layouts.extend(
        (
            ("i8", 64, (2, 2, 3), 0, False),
            ("i8", 64, (2, 2, 3), 1, False),
            ("i8", 64, (3, 21), 0, False),
            ("i1", 64, (3, 21), 0, True),
            ("i8", 64, (21, 3), 0, False),
            ("i1", 64, (21, 3), 0, True),
            ("i8", 64, (1, 64), 0, False),
            ("i1", 64, (1, 64), 0, True),
        )
    )
    return layouts


def _exhaustive_shaped_layouts() -> Iterator[ShapedLayout]:
    for (
        element_type,
        packet_lanes,
        _maximum_half_lanes,
        predicate,
    ) in _ELEMENT_FAMILIES:
        for trailing_lanes in range(1, packet_lanes + 1):
            leading_lanes = max(1, packet_lanes // trailing_lanes)
            yield (
                element_type,
                packet_lanes,
                (leading_lanes, trailing_lanes),
                0,
                predicate,
            )


def _shaped_cases(exhaustive: bool, block_route: bool) -> list[InterleaveCase]:
    layouts = list(_representative_shaped_layouts())
    if exhaustive:
        layouts.extend(_exhaustive_shaped_layouts())

    cases = {}
    for element_type, packet_lanes, half_dimensions, axis, predicate in layouts:
        trailing_lanes = 1
        for dimension in half_dimensions[axis + 1 :]:
            trailing_lanes *= dimension
        chunk_byte_count = trailing_lanes * (64 // packet_lanes)
        if _uses_block_route(chunk_byte_count) != block_route:
            continue
        combined_dimensions = list(half_dimensions)
        combined_dimensions[axis] *= 2
        half_type = _vector_type(half_dimensions, element_type)
        combined_type = _vector_type(tuple(combined_dimensions), element_type)
        shape_name = "x".join(str(dimension) for dimension in half_dimensions)
        for operation in ("interleave", "deinterleave"):
            case = InterleaveCase(
                name=f"audit_{operation}_{element_type}_{shape_name}_axis{axis}",
                operation=operation,
                half_type=half_type,
                combined_type=combined_type,
                axis=axis,
                predicate=predicate,
                chunk_byte_count=chunk_byte_count,
            )
            cases[case.name] = case
    return list(cases.values())


def _pressure_cases() -> list[InterleaveCase]:
    cases = []
    for element_type, predicate in (("i8", False), ("i1", True)):
        half_type = _vector_type((21, 3), element_type)
        combined_type = _vector_type((42, 3), element_type)
        for operation in ("interleave", "deinterleave"):
            cases.append(
                InterleaveCase(
                    name=f"pressure_{operation}_{element_type}_21x3_axis0",
                    operation=operation,
                    half_type=half_type,
                    combined_type=combined_type,
                    axis=0,
                    predicate=predicate,
                    chunk_byte_count=3,
                )
            )
    return cases


def _native_test_source(cases: list[InterleaveCase]) -> str:
    functions = "\n\n".join(case.source() for case in cases)
    checks = "\n".join(case.check() for case in cases)
    return (
        "// RUN: with-checks compile-report source-to-low,low-dce\n\n"
        "aie2p.target<core> @target\n\n"
        f"{functions}\n\n"
        "// ----\n"
        f"CHECK: COMPILE-REPORT: source_low selected_ops={len(cases)} * rows={len(cases)} *\n"
        f"{checks}\n"
    )


def _source_low_test_source(cases: list[InterleaveCase]) -> str:
    sections = (f"aie2p.target<core> @target\n\n{case.source()}" for case in cases)
    return "// RUN: emit source-low output=low\n\n" + "\n\n// ====\n\n".join(sections)


def _leaf_test_source(lowered_source: str) -> str:
    sections = []
    for section in lowered_source.split("// ===="):
        _source, low = section.split("// ----\n", maxsplit=1)
        low = low.strip()
        function_line = next(
            line for line in low.splitlines() if line.startswith("low.func.def")
        )
        function_name = function_line.rsplit("@", maxsplit=1)[1].split("(", 1)[0]
        sections.append(
            f"// RUN: emit aie2p-leaf @{function_name} report=emission\n\n"
            f"aie2p.target<core> @target\n\n{low}"
        )
    return "\n\n// ====\n\n".join(sections) + "\n"


class InterleaveFamilyTest(unittest.TestCase):
    def _run_checker(self, source_path: Path, update: bool = False):
        arguments = [_ARGS.checker]
        if update:
            arguments.append("--update")
        arguments.append(str(source_path))
        return subprocess.run(arguments, capture_output=True, text=True)

    def _check_source(self, name: str, source: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / name
            source_path.write_text(source, newline="\n")
            result = self._run_checker(source_path)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def _update_source(self, source_path: Path) -> str:
        update_result = self._run_checker(source_path, update=True)
        update_output = update_result.stderr + update_result.stdout
        self.assertIn("updated", update_output, update_output)
        result = self._run_checker(source_path)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return source_path.read_text()

    def test_native_family_selects_packet_plans(self):
        cases = _rank_one_cases(_ARGS.exhaustive) + _shaped_cases(
            _ARGS.exhaustive, block_route=False
        )
        self._check_source(
            "interleave_native_family.loom-test", _native_test_source(cases)
        )

    def test_irregular_family_selects_block_routes(self):
        cases = _shaped_cases(_ARGS.exhaustive, block_route=True)
        self._check_source(
            "interleave_block_route_family.loom-test", _native_test_source(cases)
        )

    def test_worst_case_leaf_pressure(self):
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "interleave_pressure_source.loom-test"
            source_path.write_text(
                _source_low_test_source(_pressure_cases()), newline="\n"
            )
            lowered_source = self._update_source(source_path)
            leaf_path = Path(directory) / "interleave_pressure_leaf.loom-test"
            leaf_path.write_text(_leaf_test_source(lowered_source), newline="\n")
            emitted_source = self._update_source(leaf_path)

        code_byte_counts = [
            int(count)
            for count in re.findall(r"^code bytes: ([0-9]+)$", emitted_source, re.M)
        ]
        self.assertEqual(len(code_byte_counts), len(_pressure_cases()))
        for code_byte_count in code_byte_counts:
            self.assertLessEqual(code_byte_count, _MAX_PRESSURE_CODE_BYTES)


_PARSER = argparse.ArgumentParser()
_PARSER.add_argument("checker")
_PARSER.add_argument("--exhaustive", action="store_true")
_ARGS, _UNITTEST_ARGS = _PARSER.parse_known_args()

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *_UNITTEST_ARGS])
