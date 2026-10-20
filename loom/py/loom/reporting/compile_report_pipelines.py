# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Physical pipeline inventory, separate from per-function and loop reports."""

from __future__ import annotations

from loom.reporting.compile_report import CompileReportDocument, CompileReportError

_CODE_FIELDS = {
    "program_count": "images",
    "program_code_byte_count": "unique code bytes",
    "worker_code_byte_count": "loaded code bytes",
    "minimum_worker_code_headroom_byte_count": "minimum code headroom bytes",
}
_STORAGE_FIELDS = {
    "reserved_storage_byte_count": "reserved bytes",
    "worker_storage_byte_count": "program data bytes",
    "channel_storage_byte_count": "channel bytes",
}
_WORKER_FIELDS = (
    "code_byte_count",
    "code_capacity_byte_count",
    "worker_storage_byte_count",
    "local_memory_byte_count",
    "local_memory_capacity_byte_count",
)
_MEMORY_FIELDS = (
    "reserved_byte_count",
    "program_data_byte_count",
    "occupied_byte_count",
    "high_water_byte_count",
    "capacity_byte_count",
    "maximum_bank_storage_byte_count",
    "bank_storage_capacity_byte_count",
)


def build_pipeline_show(document: CompileReportDocument) -> dict[str, object] | None:
    """Selects bounded physical facts, preserving absent logical provenance."""
    if "pipeline_plans" not in document.report:
        return None
    values = document.report["pipeline_plans"]
    if not isinstance(values, list):
        raise CompileReportError(f"{document.source}.pipeline_plans: expected array")
    plans = []
    for index, value in enumerate(values):
        path = f"{document.source}.pipeline_plans[{index}]"
        source = _object(value, path)
        plan: dict[str, object] = {
            "root": _string(source.get("root"), f"{path}.root"),
            "realization": _string(source.get("realization"), f"{path}.realization"),
        }
        for field in (*_CODE_FIELDS, *_STORAGE_FIELDS):
            if field in source:
                plan[field] = _count(source[field], f"{path}.{field}")
        for name, fields in (("workers", _WORKER_FIELDS), ("memories", _MEMORY_FIELDS)):
            if name in source:
                plan[name] = _inventory(source[name], f"{path}.{name}", name, fields)
        if "channels" in source:
            channels = _object(source["channels"], f"{path}.channels")
            plan["channel_count"] = _count(
                channels.get("count"), f"{path}.channels.count"
            )
        plans.append(plan)
    return {"count": len(plans), "rows": plans}


def _inventory(
    value: object, path: str, name: str, fields: tuple[str, ...]
) -> dict[str, object]:
    source = _object(value, path)
    count = _count(source.get("count"), f"{path}.count")
    result: dict[str, object] = {"count": count}
    if "rows" not in source:
        return result
    values = source["rows"]
    if not isinstance(values, list) or len(values) != count:
        raise CompileReportError(f"{path}.rows: expected {count} rows")
    rows = []
    index_field = "worker_index" if name == "workers" else "memory_index"
    for index, value in enumerate(values):
        row_path = f"{path}.rows[{index}]"
        source_row = _object(value, row_path)
        if _count(source_row.get(index_field), f"{row_path}.{index_field}") != index:
            raise CompileReportError(f"{row_path}.{index_field}: expected {index}")
        row: dict[str, object] = {index_field: index}
        if name == "workers":
            row["entry"] = _string(source_row.get("entry"), f"{row_path}.entry")
        if "placement" in source_row:
            placement = _object(source_row["placement"], f"{row_path}.placement")
            rank = _count(placement.get("rank"), f"{row_path}.placement.rank")
            if rank not in (1, 2, 3):
                raise CompileReportError(
                    f"{row_path}.placement.rank: expected 1, 2, or 3"
                )
            row["placement"] = [
                _count(placement.get(axis), f"{row_path}.placement.{axis}")
                for axis in ("x", "y", "z")[:rank]
            ]
        for field in fields:
            row[field] = _count(source_row.get(field), f"{row_path}.{field}")
        rows.append(row)
    result["rows"] = rows
    return result


def append_pipeline_show_text(lines: list[str], view: dict[str, object]) -> None:
    """Formats the selected inventory without inferring channels from addresses."""
    for plan in view["rows"]:
        lines.extend(("", f"Pipeline {plan['root']} ({plan['realization']})"))
        if "workers" in plan:
            lines.append(f"  workers: {plan['workers']['count']}")
        if "memories" in plan:
            lines.append(f"  physical memory owners: {plan['memories']['count']}")
        for title, fields in (("Code", _CODE_FIELDS), ("Storage", _STORAGE_FIELDS)):
            metrics = [
                f"{plan[field]} {label}"
                for field, label in fields.items()
                if field in plan
            ]
            if metrics:
                lines.append(f"  {title}: " + "; ".join(metrics))
        channels = plan.get("channel_count")
        lines.append(
            f"  logical channels: {channels if channels is not None else 'unavailable'}"
        )
        for worker in plan.get("workers", {}).get("rows", []):
            placement = _placement_text(worker)
            lines.append(
                f"  worker[{worker['worker_index']}] {worker['entry']}{placement}: "
                f"{worker['code_byte_count']}/{worker['code_capacity_byte_count']} "
                "B code; "
                f"{worker['worker_storage_byte_count']} B program data"
            )
        for memory in plan.get("memories", {}).get("rows", []):
            placement = _placement_text(memory)
            lines.append(
                f"  memory[{memory['memory_index']}]{placement}: "
                f"{memory['occupied_byte_count']}/{memory['capacity_byte_count']} "
                "B occupied; "
                f"{memory['reserved_byte_count']} B reserved + "
                f"{memory['program_data_byte_count']} B program data; "
                f"extent {memory['high_water_byte_count']} B; "
                f"peak bank {memory['maximum_bank_storage_byte_count']}/"
                f"{memory['bank_storage_capacity_byte_count']} B"
            )


def _placement_text(row: dict[str, object]) -> str:
    placement = row.get("placement")
    return (
        " at [" + ",".join(str(value) for value in placement) + "]"
        if placement is not None
        else ""
    )


def _object(value: object, path: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise CompileReportError(f"{path}: expected object")
    return value


def _count(value: object, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CompileReportError(f"{path}: expected nonnegative integer")
    return value


def _string(value: object, path: str) -> str:
    if not isinstance(value, str) or not value:
        raise CompileReportError(f"{path}: expected nonempty string")
    return value
