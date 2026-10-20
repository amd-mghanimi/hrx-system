# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Audits AIE2P static transpose selection across the carrier family."""

import argparse
import itertools
import math
import re
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class NativeMode:
    name: str
    controls: tuple[int, ...]
    unit_byte_count: int
    row_count: int
    column_count: int

    @property
    def packet_count(self) -> int:
        return len(self.controls)


# Every AIE2P shuffle mode that can realize a nonidentity axis transpose.
_NATIVE_MODES = (
    NativeMode("t8_8x4", (46,), 1, 8, 4),
    NativeMode("t8_4x8", (47,), 1, 4, 8),
    NativeMode("t8_8x8", (35,), 1, 8, 8),
    NativeMode("t8_16x4", (36,), 1, 16, 4),
    NativeMode("t8_4x16", (37,), 1, 4, 16),
    NativeMode("t16_4x2", (40,), 2, 4, 2),
    NativeMode("t16_2x4", (41,), 2, 2, 4),
    NativeMode("t16_4x4", (39,), 2, 4, 4),
    NativeMode("t16_8x2", (42,), 2, 8, 2),
    NativeMode("t16_2x8", (43,), 2, 2, 8),
    NativeMode("t16_8x4", (28,), 2, 8, 4),
    NativeMode("t16_4x8", (29,), 2, 4, 8),
    NativeMode("t16_16x2", (44,), 2, 16, 2),
    NativeMode("t16_2x16", (45,), 2, 2, 16),
    NativeMode("t32_4x4", (34,), 4, 4, 4),
    NativeMode("t8_64x2", (0, 1), 1, 64, 2),
    NativeMode("t8_2x64", (20, 21), 1, 2, 64),
    NativeMode("t16_32x2", (2, 3), 2, 32, 2),
    NativeMode("t16_2x32", (18, 19), 2, 2, 32),
    NativeMode("t16_16x4", (24, 25), 2, 16, 4),
    NativeMode("t16_4x16", (26, 27), 2, 4, 16),
    NativeMode("t16_8x8", (52, 53), 2, 8, 8),
    NativeMode("t32_16x2", (4, 5), 4, 16, 2),
    NativeMode("t32_2x16", (16, 17), 4, 2, 16),
    NativeMode("t32_8x4", (30, 31), 4, 8, 4),
    NativeMode("t32_4x8", (32, 33), 4, 4, 8),
    NativeMode("t64_8x2", (6, 7), 8, 8, 2),
    NativeMode("t64_2x8", (14, 15), 8, 2, 8),
    NativeMode("t128_4x2", (8, 9), 16, 4, 2),
    NativeMode("t128_2x4", (12, 13), 16, 2, 4),
    NativeMode("t256_2x2", (10, 11), 32, 2, 2),
)

_ELEMENT_FAMILIES = (
    ("i1", 1, 128, True),
    ("i8", 1, 128, False),
    ("f8E4M3", 1, 128, False),
    ("f8E5M2", 1, 128, False),
    ("i16", 2, 64, False),
    ("f16", 2, 64, False),
    ("bf16", 2, 64, False),
    ("i32", 4, 32, False),
    ("f32", 4, 32, False),
    ("index", 4, 32, False),
    ("offset", 4, 32, False),
    ("i64", 8, 16, False),
    ("f64", 8, 16, False),
)

_ELEMENT_LAYOUTS = {
    element_type: (lane_byte_count, maximum_lane_count, predicate)
    for element_type, lane_byte_count, maximum_lane_count, predicate in (
        _ELEMENT_FAMILIES
    )
}
_MAX_PRESSURE_CODE_BYTES = 1024
_MAX_PRESSURE_LIVE_UNITS = 12


def _vector_type(dimensions: tuple[int, ...], element_type: str) -> str:
    shape = "x".join(str(dimension) for dimension in dimensions)
    return f"vector<{shape}x{element_type}>"


def _transpose_mapping(
    dimensions: tuple[int, ...], permutation: tuple[int, ...]
) -> list[int]:
    result_dimensions = tuple(dimensions[axis] for axis in permutation)
    source_strides = []
    stride = 1
    for dimension in reversed(dimensions):
        source_strides.append(stride)
        stride *= dimension
    source_strides.reverse()

    mapping = []
    for result_ordinal in range(math.prod(result_dimensions)):
        remainder = result_ordinal
        result_indices = [0] * len(dimensions)
        for axis in range(len(dimensions) - 1, -1, -1):
            result_indices[axis] = remainder % result_dimensions[axis]
            remainder //= result_dimensions[axis]
        source_indices = [0] * len(dimensions)
        for result_axis, source_axis in enumerate(permutation):
            source_indices[source_axis] = result_indices[result_axis]
        mapping.append(
            sum(
                index * source_stride
                for index, source_stride in zip(
                    source_indices, source_strides, strict=True
                )
            )
        )
    return mapping


def _byte_mapping(mapping: list[int], lane_byte_count: int) -> list[int]:
    return [
        source_lane * lane_byte_count + lane_byte
        for source_lane in mapping
        for lane_byte in range(lane_byte_count)
    ]


def _native_mapping(mode: NativeMode, live_byte_count: int) -> list[int]:
    mapping = []
    tile_unit_count = mode.row_count * mode.column_count
    for result_byte in range(live_byte_count):
        packet_base = result_byte // 64 * 64 if mode.packet_count == 1 else 0
        packet_byte = result_byte - packet_base
        result_unit = packet_byte // mode.unit_byte_count
        unit_byte = packet_byte % mode.unit_byte_count
        tile_base = result_unit // tile_unit_count * tile_unit_count
        tile_unit = result_unit % tile_unit_count
        source_unit = (
            tile_base
            + tile_unit % mode.row_count * mode.column_count
            + tile_unit // mode.row_count
        )
        mapping.append(packet_base + source_unit * mode.unit_byte_count + unit_byte)
    return mapping


def _matching_native_mode(
    mapping: list[int], lane_byte_count: int
) -> NativeMode | None:
    desired = _byte_mapping(mapping, lane_byte_count)
    for mode in _NATIVE_MODES:
        if mode.packet_count == 2 and len(desired) <= 64:
            continue
        if _native_mapping(mode, len(desired)) == desired:
            return mode
    return None


@dataclass(frozen=True)
class TransposeCase:
    name: str
    element_type: str
    dimensions: tuple[int, ...]
    permutation: tuple[int, ...]

    @property
    def source_type(self) -> str:
        return _vector_type(self.dimensions, self.element_type)

    @property
    def result_type(self) -> str:
        result_dimensions = tuple(self.dimensions[axis] for axis in self.permutation)
        return _vector_type(result_dimensions, self.element_type)

    @property
    def plan_key(self) -> str:
        lane_byte_count, _maximum_lane_count, predicate = _ELEMENT_LAYOUTS[
            self.element_type
        ]
        mapping = _transpose_mapping(self.dimensions, self.permutation)
        if mapping == list(range(len(mapping))):
            return "transpose.alias"
        native_mode = _matching_native_mode(mapping, lane_byte_count)
        if native_mode is not None:
            suffix = "single" if native_mode.packet_count == 1 else "pair"
            return f"transpose.vshuffle-{suffix}"
        byte_4x4_mapping = [
            0,
            4,
            8,
            12,
            1,
            5,
            9,
            13,
            2,
            6,
            10,
            14,
            3,
            7,
            11,
            15,
        ]
        if lane_byte_count == 1 and mapping == byte_4x4_mapping:
            return "transpose.byte-4x4"
        suffix = "predicate-benes" if predicate else "benes"
        return f"transpose.{suffix}"

    def source(self) -> str:
        permutation = ", ".join(str(axis) for axis in self.permutation)
        return "\n".join(
            (
                f"func.def target(@target) @{self.name}(%source: {self.source_type}) "
                f"-> ({self.result_type}) {{",
                f"  %result = vector.transpose<[{permutation}]> %source : "
                f"{self.source_type} -> {self.result_type}",
                f"  func.return %result : {self.result_type}",
                "}",
            )
        )

    def check(self) -> str:
        return (
            "CHECK: COMPILE-REPORT: source_low[*] "
            f"function={self.name} source_op=vector.transpose "
            f"selection=plan plan_key={self.plan_key} *"
        )


def _boundary_cases() -> list[TransposeCase]:
    cases = []
    cross_packet_dimensions = {
        1: (8, 16),
        2: (4, 15),
        4: (2, 15),
        8: (3, 5),
    }
    for (
        element_type,
        lane_byte_count,
        maximum_lane_count,
        _predicate,
    ) in _ELEMENT_FAMILIES:
        type_name = element_type.lower()
        cases.extend(
            (
                TransposeCase(
                    f"audit_{type_name}_identity",
                    element_type,
                    (2, 3),
                    (0, 1),
                ),
                TransposeCase(
                    f"audit_{type_name}_irregular",
                    element_type,
                    (3, 5),
                    (1, 0),
                ),
                TransposeCase(
                    f"audit_{type_name}_native_pair",
                    element_type,
                    (2, maximum_lane_count // 2),
                    (1, 0),
                ),
                TransposeCase(
                    f"audit_{type_name}_cross_packet",
                    element_type,
                    cross_packet_dimensions[lane_byte_count],
                    (1, 0),
                ),
            )
        )

    for element_type in ("i1", "i8", "f8E4M3", "f8E5M2"):
        cases.append(
            TransposeCase(
                f"audit_{element_type.lower()}_byte_4x4",
                element_type,
                (4, 4),
                (1, 0),
            )
        )
    rank_15_dimensions = (1,) * 10 + (2, 2, 2, 2, 2)
    cases.append(
        TransposeCase(
            "audit_i8_rank15",
            "i8",
            rank_15_dimensions,
            tuple(range(10)) + (14, 12, 10, 13, 11),
        )
    )
    cases.extend(
        TransposeCase(
            f"audit_{element_type.lower()}_accumulator_{lane_count}",
            element_type,
            (lane_count,),
            (0,),
        )
        for element_type, lane_count in (
            ("f32", 32),
            ("i32", 64),
            ("f32", 64),
            ("index", 64),
            ("offset", 64),
            ("i64", 32),
        )
    )
    return cases


def _native_mode_case(mode: NativeMode) -> TransposeCase:
    if mode.unit_byte_count <= 8:
        element_types = {1: "i8", 2: "i16", 4: "i32", 8: "i64"}
        element_type = element_types[mode.unit_byte_count]
        dimensions = (mode.row_count, mode.column_count)
        permutation = (1, 0)
    else:
        element_type = "i64"
        dimensions = (
            mode.row_count,
            mode.column_count,
            mode.unit_byte_count // 8,
        )
        permutation = (1, 0, 2)
    return TransposeCase(f"native_{mode.name}", element_type, dimensions, permutation)


def _pressure_cases() -> list[TransposeCase]:
    return [
        TransposeCase("pressure_i8", "i8", (2, 2, 2, 2, 3), (0, 1, 3, 4, 2)),
        TransposeCase("pressure_i16", "i16", (2, 2, 2, 2, 3), (0, 1, 3, 4, 2)),
        TransposeCase("pressure_i32", "i32", (2, 2, 2, 2, 2), (1, 2, 4, 0, 3)),
        TransposeCase("pressure_i64", "i64", (2, 2, 2, 2), (1, 3, 0, 2)),
        TransposeCase("pressure_i1", "i1", (8, 16), (1, 0)),
    ]


def _ordered_shapes(
    product_limit: int, maximum_rank: int = 5
) -> Iterator[tuple[int, ...]]:
    def extend(prefix: tuple[int, ...], product: int) -> Iterator[tuple[int, ...]]:
        if len(prefix) >= 2:
            yield prefix
        if len(prefix) == maximum_rank:
            return
        for dimension in range(2, product_limit // product + 1):
            yield from extend(prefix + (dimension,), product * dimension)

    yield from extend((), 1)


def _exhaustive_cases() -> Iterator[TransposeCase]:
    ordinal = 0
    for element_type, maximum_lane_count in (
        ("i8", 128),
        ("i16", 64),
        ("i32", 32),
        ("i64", 16),
    ):
        for dimensions in _ordered_shapes(maximum_lane_count):
            for permutation in itertools.permutations(range(len(dimensions))):
                yield TransposeCase(
                    f"exhaustive_{element_type}_{ordinal}",
                    element_type,
                    dimensions,
                    permutation,
                )
                ordinal += 1


def _report_source(cases: list[TransposeCase]) -> str:
    functions = "\n\n".join(case.source() for case in cases)
    checks = "\n".join(case.check() for case in cases)
    return (
        "// RUN: with-checks compile-report source-to-low,low-dce\n\n"
        "aie2p.target<core> @target\n\n"
        f"{functions}\n\n"
        "// ----\n"
        f"CHECK: COMPILE-REPORT: source_low selected_ops={len(cases)} * "
        f"rows={len(cases)} *\n"
        f"{checks}\n"
    )


def _source_low_source(cases: list[TransposeCase]) -> str:
    sections = (f"aie2p.target<core> @target\n\n{case.source()}" for case in cases)
    return "// RUN: emit source-low output=low\n\n" + "\n\n// ====\n\n".join(sections)


def _low_test_source(lowered_source: str, run_template: str) -> str:
    sections = []
    for section in lowered_source.split("// ===="):
        _source, low = section.split("// ----\n", maxsplit=1)
        low = low.strip()
        function_line = next(
            line for line in low.splitlines() if line.startswith("low.func.def")
        )
        function_name = function_line.rsplit("@", maxsplit=1)[1].split("(", 1)[0]
        sections.append(
            f"// RUN: {run_template.format(function_name=function_name)}\n\n"
            f"aie2p.target<core> @target\n\n{low}"
        )
    return "\n\n// ====\n\n".join(sections) + "\n"


class TransposeFamilyTest(unittest.TestCase):
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

    def test_boundary_family_selects_packet_plans(self):
        cases = _boundary_cases()
        self.assertEqual(
            {case.plan_key for case in cases},
            {
                "transpose.alias",
                "transpose.vshuffle-pair",
                "transpose.byte-4x4",
                "transpose.benes",
                "transpose.predicate-benes",
            },
        )
        self._check_source("transpose_family.loom-test", _report_source(cases))

    def test_every_native_mode_uses_its_control(self):
        cases = [_native_mode_case(mode) for mode in _NATIVE_MODES]
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "transpose_native_modes.loom-test"
            source_path.write_text(_source_low_source(cases), newline="\n")
            lowered_source = self._update_source(source_path)

        sections = lowered_source.split("// ====")
        self.assertEqual(len(sections), len(_NATIVE_MODES))
        for mode, case, section in zip(_NATIVE_MODES, cases, sections, strict=True):
            self.assertIn(f"@{case.name}", section)
            _source, low = section.split("// ----\n", maxsplit=1)
            controls = tuple(
                int(value) for value in re.findall(r"= mova\.i32 ([0-9]+)$", low, re.M)
            )
            self.assertEqual(controls, mode.controls, mode.name)
            self.assertEqual(low.count(" = vshuffle "), mode.packet_count, mode.name)
            self.assertNotIn(" = vshift ", low, mode.name)
            self.assertNotIn(" = vsel.8 ", low, mode.name)

    def test_worst_case_leaf_pressure(self):
        pressure_cases = _pressure_cases()
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "transpose_pressure_source.loom-test"
            source_path.write_text(_source_low_source(pressure_cases), newline="\n")
            lowered_source = self._update_source(source_path)

            leaf_path = Path(directory) / "transpose_pressure_leaf.loom-test"
            leaf_path.write_text(
                _low_test_source(
                    lowered_source,
                    "emit aie2p-leaf @{function_name} report=emission",
                ),
                newline="\n",
            )
            emitted_source = self._update_source(leaf_path)

            report_path = Path(directory) / "transpose_pressure_report.loom-test"
            report_path.write_text(
                _low_test_source(
                    lowered_source, "emit low-compile-report @{function_name}"
                ),
                newline="\n",
            )
            report_source = self._update_source(report_path)

        code_byte_counts = [
            int(count)
            for count in re.findall(r"^code bytes: ([0-9]+)$", emitted_source, re.M)
        ]
        self.assertEqual(len(code_byte_counts), len(pressure_cases))
        self.assertLessEqual(max(code_byte_counts), _MAX_PRESSURE_CODE_BYTES)

        peak_live_units = [
            int(count)
            for count in re.findall(r"peak_live_units=([0-9]+)", report_source)
        ]
        self.assertEqual(len(peak_live_units), len(pressure_cases))
        self.assertLessEqual(max(peak_live_units), _MAX_PRESSURE_LIVE_UNITS)
        self.assertEqual(
            len(re.findall(r"allocation assignments=.* spills=0 ", report_source)),
            len(pressure_cases),
        )
        self.assertNotRegex(report_source, r"materialized_reloads=[1-9]")

    def test_exhaustive_shape_and_permutation_matrix(self):
        if not _ARGS.exhaustive:
            self.skipTest("manual exhaustive mode")
        batch = []
        batch_ordinal = 0
        for case in _exhaustive_cases():
            batch.append(case)
            if len(batch) != 1024:
                continue
            self._check_source(
                f"transpose_exhaustive_{batch_ordinal}.loom-test",
                _report_source(batch),
            )
            batch = []
            batch_ordinal += 1
        if batch:
            self._check_source(
                f"transpose_exhaustive_{batch_ordinal}.loom-test",
                _report_source(batch),
            )


_PARSER = argparse.ArgumentParser()
_PARSER.add_argument("checker")
_PARSER.add_argument("--exhaustive", action="store_true")
_ARGS, _UNITTEST_ARGS = _PARSER.parse_known_args()

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *_UNITTEST_ARGS])
