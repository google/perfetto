#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Computes class-level and dominator-path diffs between two heap dumps.

Implements the mandatory Perfetto heap diffing guardrails:
1. Uses android_heap_graph_class_aggregation (never raw
   SUM(dominated_size_bytes) over heap_graph_dominator_tree) to prevent
   double-counting.
2. Scopes strictly by process_name and latest graph_sample_ts per trace.
3. Joins dominator paths on (path, heap_type) without [self_count] in node
   labels.
4. Highlights both total dominated byte growth and reachable object count growth
   so small wrapper object leaks (e.g. Binder subclasses /
   ContentObserver$Transport) are surfaced alongside large byte-dominating
   leaks.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
from dataclasses import dataclass
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Dict, Iterable, List, Optional, Tuple

DEFAULT_TOP_N = 10
SORT_BY_DOMINATED_SIZE = "dominated_size"
SORT_BY_REACHABLE_COUNT = "reachable_count"
SORT_BY_REACHABLE_SIZE = "reachable_size"
SORT_BY_NATIVE_SIZE = "native_size"
VALID_SORT_KEYS = (
    SORT_BY_DOMINATED_SIZE,
    SORT_BY_REACHABLE_COUNT,
    SORT_BY_REACHABLE_SIZE,
    SORT_BY_NATIVE_SIZE,
)


@dataclass(frozen=True)
class ClassStats:
  """Class-level aggregation metrics for a single heap dump."""

  process_name: str
  upid: int
  graph_sample_ts: int
  class_name: str
  is_libcore_or_array: bool
  obj_count: int
  size_bytes: int
  native_size_bytes: int
  reachable_obj_count: int
  reachable_size_bytes: int
  reachable_native_size_bytes: int
  dominated_obj_count: int
  dominated_size_bytes: int
  dominated_native_size_bytes: int

  @property
  def total_dominated_bytes(self) -> int:
    return self.dominated_size_bytes + self.dominated_native_size_bytes


@dataclass(frozen=True)
class ClassDiffRow:
  """Before/after comparison for a single Java class."""

  class_name: str
  before_reachable_count: int
  after_reachable_count: int
  before_reachable_size_bytes: int
  after_reachable_size_bytes: int
  before_native_size_bytes: int
  after_native_size_bytes: int
  before_total_dominated_bytes: int
  after_total_dominated_bytes: int

  @property
  def delta_reachable_count(self) -> int:
    return self.after_reachable_count - self.before_reachable_count

  @property
  def delta_reachable_size_bytes(self) -> int:
    return self.after_reachable_size_bytes - self.before_reachable_size_bytes

  @property
  def delta_native_size_bytes(self) -> int:
    return self.after_native_size_bytes - self.before_native_size_bytes

  @property
  def delta_total_dominated_bytes(self) -> int:
    return self.after_total_dominated_bytes - self.before_total_dominated_bytes


@dataclass(frozen=True)
class DominatorPathStats:
  """Dominator class tree path stats for a single heap dump."""

  process_name: str
  upid: int
  graph_sample_ts: int
  path: str
  heap_type: str
  class_name: str
  self_count: int
  self_size: int


@dataclass(frozen=True)
class DominatorPathDiffRow:
  """Before/after comparison for a single (path, heap_type) signature."""

  path: str
  heap_type: str
  class_name: str
  before_self_count: int
  after_self_count: int
  before_self_size: int
  after_self_size: int

  @property
  def delta_self_count(self) -> int:
    return self.after_self_count - self.before_self_count

  @property
  def delta_self_size(self) -> int:
    return self.after_self_size - self.before_self_size


@dataclass(frozen=True)
class FieldValueStats:
  """String/primitive field value distribution stats for a single heap dump."""

  process_name: str
  upid: int
  graph_sample_ts: int
  heap_type: str
  class_name: str
  field_name: str
  field_type: str
  field_value: str
  instance_count: int
  self_size: int
  dominated_size: int


@dataclass(frozen=True)
class FieldValueDiffRow:
  """Before/after comparison for a (class_name, field_name, field_value)."""

  class_name: str
  field_name: str
  field_type: str
  field_value: str
  before_instance_count: int
  after_instance_count: int
  before_self_size: int
  after_self_size: int
  before_dominated_size: int
  after_dominated_size: int

  @property
  def delta_instance_count(self) -> int:
    return self.after_instance_count - self.before_instance_count

  @property
  def delta_self_size(self) -> int:
    return self.after_self_size - self.before_self_size

  @property
  def delta_dominated_size(self) -> int:
    return self.after_dominated_size - self.before_dominated_size


@dataclass(frozen=True)
class InstanceConfigStats:
  """Per-instance field configuration tuple stats for a single heap dump."""

  process_name: str
  upid: int
  graph_sample_ts: int
  heap_type: str
  class_name: str
  config_tuple: str
  instance_count: int
  self_size: int
  dominated_size: int


@dataclass(frozen=True)
class InstanceConfigDiffRow:
  """Before/after comparison for a (class_name, config_tuple)."""

  class_name: str
  config_tuple: str
  before_instance_count: int
  after_instance_count: int
  before_self_size: int
  after_self_size: int
  before_dominated_size: int
  after_dominated_size: int

  @property
  def delta_instance_count(self) -> int:
    return self.after_instance_count - self.before_instance_count

  @property
  def delta_self_size(self) -> int:
    return self.after_self_size - self.before_self_size

  @property
  def delta_dominated_size(self) -> int:
    return self.after_dominated_size - self.before_dominated_size


def _get_sql_file_path(filename: str) -> Path:
  """Resolves the path to a companion SQL script (preserving runfiles symlinks)."""
  candidate = Path(__file__).parent / filename
  if candidate.exists():
    return candidate
  resolved = Path(__file__).resolve().parent / filename
  if resolved.exists():
    return resolved
  raise FileNotFoundError(f"SQL file not found: {candidate}")


def _load_sql_file(filename: str) -> str:
  """Loads a companion SQL file from the script directory."""
  return _get_sql_file_path(filename).read_text(encoding="utf-8")


def _parse_int(value: Optional[str]) -> int:
  if not value or value == "[NULL]":
    return 0
  try:
    return int(float(value))
  except ValueError:
    return 0


def _run_trace_processor_query(trace_processor_bin: str, trace_path: Path,
                               sql_file_path: Path) -> List[Dict[str, str]]:
  """Runs a SQL query file against a trace using trace_processor."""
  if not trace_path.exists():
    raise FileNotFoundError(f"Trace file not found: {trace_path}")
  if not sql_file_path.exists():
    raise FileNotFoundError(f"SQL query file not found: {sql_file_path}")

  cmd = [trace_processor_bin, "-q", str(sql_file_path), str(trace_path)]
  result = subprocess.run(
      cmd,
      capture_output=True,
      text=True,
      encoding="utf-8",
      errors="replace",
      check=False,
  )
  if result.returncode != 0:
    raise RuntimeError(
        f"trace_processor failed (code {result.returncode}):\n{result.stderr}")

  reader = csv.DictReader(io.StringIO(result.stdout))
  return list(reader)


def _run_all_queries_on_trace(
    trace_processor_bin: str,
    trace_path: Path,
    class_sql_path: Path,
    paths_sql_path: Path,
    field_values_sql_path: Path,
) -> Tuple[List[Dict[str, str]], List[Dict[str, str]], List[Dict[str, str]]]:
  """Runs class, dominator path, and field value queries in a single trace_processor call."""
  if not trace_path.exists():
    raise FileNotFoundError(f"Trace file not found: {trace_path}")

  combined_sql = (
      class_sql_path.read_text(encoding="utf-8") + "\n;\n" +
      paths_sql_path.read_text(encoding="utf-8") + "\n;\n" +
      field_values_sql_path.read_text(encoding="utf-8"))

  with tempfile.NamedTemporaryFile(
      mode="w", suffix=".sql", encoding="utf-8", delete=False) as tmp:
    tmp.write(combined_sql)
    tmp_path = Path(tmp.name)

  try:
    cmd = [trace_processor_bin, "-q", str(tmp_path), str(trace_path)]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
      raise RuntimeError(
          f"trace_processor failed (code {result.returncode}):\n{result.stderr}"
      )
  finally:
    tmp_path.unlink(missing_ok=True)

  blocks = [b.strip() for b in result.stdout.strip().split("\n\n") if b.strip()]
  class_rows = (
      list(csv.DictReader(io.StringIO(blocks[0]))) if len(blocks) > 0 else [])
  path_rows = (
      list(csv.DictReader(io.StringIO(blocks[1]))) if len(blocks) > 1 else [])
  field_value_rows = (
      list(csv.DictReader(io.StringIO(blocks[2]))) if len(blocks) > 2 else [])
  return class_rows, path_rows, field_value_rows


def parse_class_stats(rows: Iterable[Dict[str, str]]) -> List[ClassStats]:
  """Parses CSV dictionary rows into ClassStats objects."""
  parsed: List[ClassStats] = []
  for row in rows:
    parsed.append(
        ClassStats(
            process_name=row.get("process_name", ""),
            upid=_parse_int(row.get("upid")),
            graph_sample_ts=_parse_int(row.get("graph_sample_ts")),
            class_name=row.get("class_name", ""),
            is_libcore_or_array=bool(
                _parse_int(row.get("is_libcore_or_array"))),
            obj_count=_parse_int(row.get("obj_count")),
            size_bytes=_parse_int(row.get("size_bytes")),
            native_size_bytes=_parse_int(row.get("native_size_bytes")),
            reachable_obj_count=_parse_int(row.get("reachable_obj_count")),
            reachable_size_bytes=_parse_int(row.get("reachable_size_bytes")),
            reachable_native_size_bytes=_parse_int(
                row.get("reachable_native_size_bytes")),
            dominated_obj_count=_parse_int(row.get("dominated_obj_count")),
            dominated_size_bytes=_parse_int(row.get("dominated_size_bytes")),
            dominated_native_size_bytes=_parse_int(
                row.get("dominated_native_size_bytes")),
        ))
  return parsed


def parse_dominator_paths(
    rows: Iterable[Dict[str, str]],) -> List[DominatorPathStats]:
  """Parses CSV dictionary rows into DominatorPathStats objects."""
  parsed: List[DominatorPathStats] = []
  for row in rows:
    parsed.append(
        DominatorPathStats(
            process_name=row.get("process_name", ""),
            upid=_parse_int(row.get("upid")),
            graph_sample_ts=_parse_int(row.get("graph_sample_ts")),
            path=row.get("path", ""),
            heap_type=row.get("heap_type", ""),
            class_name=row.get("class_name", ""),
            self_count=_parse_int(row.get("self_count")),
            self_size=_parse_int(row.get("self_size")),
        ))
  return parsed


def parse_field_value_and_config_stats(
    rows: Iterable[Dict[str, str]],
) -> Tuple[List[FieldValueStats], List[InstanceConfigStats]]:
  """Parses CSV rows into FieldValueStats and InstanceConfigStats."""
  field_values: List[FieldValueStats] = []
  instance_configs: List[InstanceConfigStats] = []
  for row in rows:
    section = row.get("section", "")
    if section == "field_value":
      field_values.append(
          FieldValueStats(
              process_name=row.get("process_name", ""),
              upid=_parse_int(row.get("upid")),
              graph_sample_ts=_parse_int(row.get("graph_sample_ts")),
              heap_type=row.get("heap_type", ""),
              class_name=row.get("class_name", ""),
              field_name=row.get("field_name", ""),
              field_type=row.get("field_type", ""),
              field_value=row.get("field_value", ""),
              instance_count=_parse_int(row.get("instance_count")),
              self_size=_parse_int(row.get("self_size")),
              dominated_size=_parse_int(row.get("dominated_size")),
          ))
    elif section == "instance_config":
      instance_configs.append(
          InstanceConfigStats(
              process_name=row.get("process_name", ""),
              upid=_parse_int(row.get("upid")),
              graph_sample_ts=_parse_int(row.get("graph_sample_ts")),
              heap_type=row.get("heap_type", ""),
              class_name=row.get("class_name", ""),
              config_tuple=row.get("config_tuple", ""),
              instance_count=_parse_int(row.get("instance_count")),
              self_size=_parse_int(row.get("self_size")),
              dominated_size=_parse_int(row.get("dominated_size")),
          ))
  return field_values, instance_configs


def list_process_dump_timestamps(
    rows: List[ClassStats],
    process_name: str,
) -> List[int]:
  """Returns sorted unique graph_sample_ts values for a process in a trace."""
  return sorted(
      {r.graph_sample_ts for r in rows if r.process_name == process_name})


def select_process_and_latest_ts(
    rows: List[ClassStats],
    process_name: Optional[str] = None,
    explicit_ts: Optional[int] = None,
    explicit_upid: Optional[int] = None,
    dump_index: int = -1,
) -> Tuple[str, int, int, List[ClassStats]]:
  """Scopes ClassStats to a target process and a selected (graph_sample_ts, upid)."""
  if not rows:
    raise ValueError("No class aggregation rows found in trace output.")

  available_processes = sorted({r.process_name for r in rows})
  selected_process = process_name or available_processes[0]
  process_rows = [r for r in rows if r.process_name == selected_process]
  if not process_rows and process_name:
    matching = [
        p for p in available_processes if process_name in p or p in process_name
    ]
    if matching:
      selected_process = matching[0]
      process_rows = [r for r in rows if r.process_name == selected_process]
  if (not process_rows and len(available_processes) == 1 and
      available_processes[0].startswith("pid=")):
    selected_process = available_processes[0]
    process_rows = rows
  if not process_rows:
    raise ValueError(
        f"Process '{selected_process}' not found. Available processes:"
        f" {available_processes}")

  dumps = sorted({(r.graph_sample_ts, r.upid) for r in process_rows})
  if explicit_ts is not None:
    dumps = [d for d in dumps if d[0] == explicit_ts]
  if explicit_upid is not None:
    dumps = [d for d in dumps if d[1] == explicit_upid]
  if not dumps:
    raise ValueError(
        f"No matching heap dump found for process '{selected_process}'"
        f" (ts={explicit_ts}, upid={explicit_upid}).")

  try:
    selected_ts, selected_upid = dumps[dump_index]
  except IndexError as exc:
    raise ValueError(f"Heap dump index {dump_index} out of range for process"
                     f" '{selected_process}' with {len(dumps)} available dumps:"
                     f" {[d[0] for d in dumps]}") from exc
  scoped_rows = [
      r for r in process_rows
      if r.graph_sample_ts == selected_ts and r.upid == selected_upid
  ]
  return selected_process, selected_upid, selected_ts, scoped_rows


def select_dominator_paths_for_dump(
    rows: List[DominatorPathStats],
    process_name: str,
    graph_sample_ts: int,
    upid: Optional[int] = None,
) -> List[DominatorPathStats]:
  return [
      r for r in rows if r.process_name == process_name and
      r.graph_sample_ts == graph_sample_ts and (upid is None or r.upid == upid)
  ]


def select_field_values_for_dump(
    rows: List[FieldValueStats],
    process_name: str,
    graph_sample_ts: int,
    upid: Optional[int] = None,
) -> List[FieldValueStats]:
  return [
      r for r in rows if r.process_name == process_name and
      r.graph_sample_ts == graph_sample_ts and (upid is None or r.upid == upid)
  ]


def select_instance_configs_for_dump(
    rows: List[InstanceConfigStats],
    process_name: str,
    graph_sample_ts: int,
    upid: Optional[int] = None,
) -> List[InstanceConfigStats]:
  return [
      r for r in rows if r.process_name == process_name and
      r.graph_sample_ts == graph_sample_ts and (upid is None or r.upid == upid)
  ]


def _aggregate_class_rows(rows: Iterable[ClassStats],) -> Dict[str, ClassStats]:
  """Aggregates ClassStats by class_name to prevent silent overwrite across classloaders."""
  aggregated: Dict[str, ClassStats] = {}
  for r in rows:
    existing = aggregated.get(r.class_name)
    if existing is None:
      aggregated[r.class_name] = r
      continue
    aggregated[r.class_name] = ClassStats(
        process_name=existing.process_name,
        upid=existing.upid,
        graph_sample_ts=existing.graph_sample_ts,
        class_name=existing.class_name,
        is_libcore_or_array=existing.is_libcore_or_array,
        obj_count=existing.obj_count + r.obj_count,
        size_bytes=existing.size_bytes + r.size_bytes,
        native_size_bytes=existing.native_size_bytes + r.native_size_bytes,
        reachable_obj_count=existing.reachable_obj_count +
        r.reachable_obj_count,
        reachable_size_bytes=existing.reachable_size_bytes +
        r.reachable_size_bytes,
        reachable_native_size_bytes=existing.reachable_native_size_bytes +
        r.reachable_native_size_bytes,
        dominated_obj_count=existing.dominated_obj_count +
        r.dominated_obj_count,
        dominated_size_bytes=existing.dominated_size_bytes +
        r.dominated_size_bytes,
        dominated_native_size_bytes=existing.dominated_native_size_bytes +
        r.dominated_native_size_bytes,
    )
  return aggregated


def compute_class_diffs(
    before_rows: List[ClassStats],
    after_rows: List[ClassStats],
    class_filter: Optional[str] = None,
) -> List[ClassDiffRow]:
  """Computes before/after diffs keyed by class_name."""
  before_map = _aggregate_class_rows(before_rows)
  after_map = _aggregate_class_rows(after_rows)
  all_classes = sorted(set(before_map.keys()) | set(after_map.keys()))

  diffs: List[ClassDiffRow] = []
  for cls in all_classes:
    if class_filter and class_filter.lower() not in cls.lower():
      continue
    b = before_map.get(cls)
    a = after_map.get(cls)
    diffs.append(
        ClassDiffRow(
            class_name=cls,
            before_reachable_count=b.reachable_obj_count if b else 0,
            after_reachable_count=a.reachable_obj_count if a else 0,
            before_reachable_size_bytes=b.reachable_size_bytes if b else 0,
            after_reachable_size_bytes=a.reachable_size_bytes if a else 0,
            before_native_size_bytes=b.reachable_native_size_bytes if b else 0,
            after_native_size_bytes=a.reachable_native_size_bytes if a else 0,
            before_total_dominated_bytes=b.total_dominated_bytes if b else 0,
            after_total_dominated_bytes=a.total_dominated_bytes if a else 0,
        ))
  return diffs


def _aggregate_dominator_paths(
    rows: Iterable[DominatorPathStats],
) -> Dict[Tuple[str, str], DominatorPathStats]:
  """Aggregates DominatorPathStats by (path, heap_type) to avoid overwrites."""
  aggregated: Dict[Tuple[str, str], DominatorPathStats] = {}
  for r in rows:
    key = (r.path, r.heap_type)
    existing = aggregated.get(key)
    if existing is None:
      aggregated[key] = r
      continue
    aggregated[key] = DominatorPathStats(
        process_name=existing.process_name,
        upid=existing.upid,
        graph_sample_ts=existing.graph_sample_ts,
        path=existing.path,
        heap_type=existing.heap_type,
        class_name=existing.class_name,
        self_count=existing.self_count + r.self_count,
        self_size=existing.self_size + r.self_size,
    )
  return aggregated


def compute_dominator_path_diffs(
    before_paths: List[DominatorPathStats],
    after_paths: List[DominatorPathStats],
    class_filter: Optional[str] = None,
) -> List[DominatorPathDiffRow]:
  """Computes before/after diffs keyed by (path, heap_type)."""
  before_map = _aggregate_dominator_paths(before_paths)
  after_map = _aggregate_dominator_paths(after_paths)
  all_keys = sorted(set(before_map.keys()) | set(after_map.keys()))

  diffs: List[DominatorPathDiffRow] = []
  for path_key, heap_type in all_keys:
    if class_filter and class_filter.lower() not in path_key.lower():
      continue
    b = before_map.get((path_key, heap_type))
    a = after_map.get((path_key, heap_type))
    class_name = (a.class_name if a else b.class_name) if (a or b) else ""
    diffs.append(
        DominatorPathDiffRow(
            path=path_key,
            heap_type=heap_type,
            class_name=class_name,
            before_self_count=b.self_count if b else 0,
            after_self_count=a.self_count if a else 0,
            before_self_size=b.self_size if b else 0,
            after_self_size=a.self_size if a else 0,
        ))
  return diffs


def _aggregate_field_values(
    rows: Iterable[FieldValueStats],
) -> Dict[Tuple[str, str, str, str], FieldValueStats]:
  aggregated: Dict[Tuple[str, str, str, str], FieldValueStats] = {}
  for r in rows:
    key = (r.class_name, r.field_name, r.field_type, r.field_value)
    existing = aggregated.get(key)
    if existing is None:
      aggregated[key] = r
      continue
    aggregated[key] = FieldValueStats(
        process_name=existing.process_name,
        upid=existing.upid,
        graph_sample_ts=existing.graph_sample_ts,
        heap_type=existing.heap_type,
        class_name=existing.class_name,
        field_name=existing.field_name,
        field_type=existing.field_type,
        field_value=existing.field_value,
        instance_count=existing.instance_count + r.instance_count,
        self_size=existing.self_size + r.self_size,
        dominated_size=existing.dominated_size + r.dominated_size,
    )
  return aggregated


def compute_field_value_diffs(
    before_rows: List[FieldValueStats],
    after_rows: List[FieldValueStats],
    class_filter: Optional[str] = None,
) -> List[FieldValueDiffRow]:
  """Computes before/after diffs keyed by (class_name, field_name, field_type, field_value)."""
  before_map = _aggregate_field_values(before_rows)
  after_map = _aggregate_field_values(after_rows)
  all_keys = sorted(set(before_map.keys()) | set(after_map.keys()))

  diffs: List[FieldValueDiffRow] = []
  for cls, field_name, field_type, field_value in all_keys:
    if class_filter and class_filter.lower() not in cls.lower():
      continue
    b = before_map.get((cls, field_name, field_type, field_value))
    a = after_map.get((cls, field_name, field_type, field_value))
    diffs.append(
        FieldValueDiffRow(
            class_name=cls,
            field_name=field_name,
            field_type=field_type,
            field_value=field_value,
            before_instance_count=b.instance_count if b else 0,
            after_instance_count=a.instance_count if a else 0,
            before_self_size=b.self_size if b else 0,
            after_self_size=a.self_size if a else 0,
            before_dominated_size=b.dominated_size if b else 0,
            after_dominated_size=a.dominated_size if a else 0,
        ))
  return diffs


def _aggregate_instance_configs(
    rows: Iterable[InstanceConfigStats],
) -> Dict[Tuple[str, str], InstanceConfigStats]:
  aggregated: Dict[Tuple[str, str], InstanceConfigStats] = {}
  for r in rows:
    key = (r.class_name, r.config_tuple)
    existing = aggregated.get(key)
    if existing is None:
      aggregated[key] = r
      continue
    aggregated[key] = InstanceConfigStats(
        process_name=existing.process_name,
        upid=existing.upid,
        graph_sample_ts=existing.graph_sample_ts,
        heap_type=existing.heap_type,
        class_name=existing.class_name,
        config_tuple=existing.config_tuple,
        instance_count=existing.instance_count + r.instance_count,
        self_size=existing.self_size + r.self_size,
        dominated_size=existing.dominated_size + r.dominated_size,
    )
  return aggregated


def compute_instance_config_diffs(
    before_rows: List[InstanceConfigStats],
    after_rows: List[InstanceConfigStats],
    class_filter: Optional[str] = None,
) -> List[InstanceConfigDiffRow]:
  """Computes before/after diffs keyed by (class_name, config_tuple)."""
  before_map = _aggregate_instance_configs(before_rows)
  after_map = _aggregate_instance_configs(after_rows)
  all_keys = sorted(set(before_map.keys()) | set(after_map.keys()))

  diffs: List[InstanceConfigDiffRow] = []
  for cls, config_tuple in all_keys:
    if class_filter and class_filter.lower() not in cls.lower():
      continue
    b = before_map.get((cls, config_tuple))
    a = after_map.get((cls, config_tuple))
    diffs.append(
        InstanceConfigDiffRow(
            class_name=cls,
            config_tuple=config_tuple,
            before_instance_count=b.instance_count if b else 0,
            after_instance_count=a.instance_count if a else 0,
            before_self_size=b.self_size if b else 0,
            after_self_size=a.self_size if a else 0,
            before_dominated_size=b.dominated_size if b else 0,
            after_dominated_size=a.dominated_size if a else 0,
        ))
  return diffs


def _format_signed(val: int) -> str:
  return f"+{val:,}" if val > 0 else f"{val:,}"


def format_markdown_report(
    process_name: str,
    before_ts: int,
    after_ts: int,
    class_diffs: List[ClassDiffRow],
    path_diffs: List[DominatorPathDiffRow],
    top_n: int,
    sort_by: str,
    before_available_ts: Optional[List[int]] = None,
    after_available_ts: Optional[List[int]] = None,
    field_value_diffs: Optional[List[FieldValueDiffRow]] = None,
    instance_config_diffs: Optional[List[InstanceConfigDiffRow]] = None,
) -> str:
  """Generates a Markdown diff report for classes, dominator paths, and field values."""
  total_before_bytes = sum(r.before_reachable_size_bytes for r in class_diffs)
  total_after_bytes = sum(r.after_reachable_size_bytes for r in class_diffs)
  delta_bytes = total_after_bytes - total_before_bytes

  total_before_objs = sum(r.before_reachable_count for r in class_diffs)
  total_after_objs = sum(r.after_reachable_count for r in class_diffs)
  delta_objs = total_after_objs - total_before_objs

  before_ts_detail = f"`{before_ts}`"
  if before_available_ts and len(before_available_ts) > 1:
    idx = (
        before_available_ts.index(before_ts) +
        1 if before_ts in before_available_ts else "?")
    before_ts_detail += (
        f" (selected #{idx} of {len(before_available_ts)} available dumps:"
        f" `{before_available_ts}`)")

  after_ts_detail = f"`{after_ts}`"
  if after_available_ts and len(after_available_ts) > 1:
    idx = (
        after_available_ts.index(after_ts) +
        1 if after_ts in after_available_ts else "?")
    after_ts_detail += (
        f" (selected #{idx} of {len(after_available_ts)} available dumps:"
        f" `{after_available_ts}`)")

  lines: List[str] = [
      f"# Heap Graph Diff Report: `{process_name}`",
      "",
      f"- **Baseline Timestamp (`before`)**: {before_ts_detail}",
      f"- **Candidate Timestamp (`after`)**: {after_ts_detail}",
      (f"- **Total Reachable Shallow Heap**: `{total_before_bytes:,} B` ->"
       f" `{total_after_bytes:,} B` (`{_format_signed(delta_bytes)} B`)"),
      (f"- **Total Reachable Objects**: `{total_before_objs:,}` ->"
       f" `{total_after_objs:,}` (`{_format_signed(delta_objs)}`)"),
      "",
  ]

  def _sort_key_for_class(row: ClassDiffRow) -> Tuple[int, int]:
    if sort_by == SORT_BY_REACHABLE_COUNT:
      return (row.delta_reachable_count, row.delta_total_dominated_bytes)
    if sort_by == SORT_BY_REACHABLE_SIZE:
      return (row.delta_reachable_size_bytes, row.delta_reachable_count)
    if sort_by == SORT_BY_NATIVE_SIZE:
      return (row.delta_native_size_bytes, row.delta_total_dominated_bytes)
    return (row.delta_total_dominated_bytes, row.delta_reachable_count)

  sorted_by_primary = sorted(
      class_diffs, key=_sort_key_for_class, reverse=True)[:top_n]

  lines.extend([
      f"## Top {top_n} Regressed Classes (Sorted by `{sort_by}`)",
      "",
      ("| Class Name | Reachable Obj Delta (`Before -> After`) | Shallow"
       " Size Delta (`B`) | Dominated Size Delta (`B`) |"),
      "| :--- | :--- | :--- | :--- |",
  ])
  for row in sorted_by_primary:
    if row.delta_reachable_count <= 0 and row.delta_total_dominated_bytes <= 0:
      continue
    lines.append(
        f"| `{row.class_name}` | `{_format_signed(row.delta_reachable_count)}`"
        f" (`{row.before_reachable_count:,} -> {row.after_reachable_count:,}`)"
        f" | `{_format_signed(row.delta_reachable_size_bytes)}` |"
        f" `{_format_signed(row.delta_total_dominated_bytes)}` |")

  if sort_by != SORT_BY_REACHABLE_COUNT:
    sorted_by_count = sorted(
        class_diffs,
        key=lambda r: (r.delta_reachable_count, r.delta_total_dominated_bytes),
        reverse=True,
    )[:top_n]
    lines.extend([
        "",
        (f"## Top {top_n} Regressed Classes by Instance Count (Catches"
         " Wrapper / Binder Leaks)"),
        "",
        ("| Class Name | Reachable Obj Delta (`Before -> After`) | Shallow"
         " Size Delta (`B`) | Dominated Size Delta (`B`) |"),
        "| :--- | :--- | :--- | :--- |",
    ])
    for row in sorted_by_count:
      if row.delta_reachable_count <= 0:
        continue
      lines.append(f"| `{row.class_name}` |"
                   f" `{_format_signed(row.delta_reachable_count)}`"
                   f" (`{row.before_reachable_count:,} ->"
                   f" {row.after_reachable_count:,}`) |"
                   f" `{_format_signed(row.delta_reachable_size_bytes)}` |"
                   f" `{_format_signed(row.delta_total_dominated_bytes)}` |")

  if path_diffs:
    sorted_paths = sorted(
        path_diffs,
        key=lambda r: (r.delta_self_size, r.delta_self_count),
        reverse=True,
    )[:top_n]
    lines.extend([
        "",
        f"## Top {top_n} Regressed Dominator Class Paths",
        "",
        ("| Dominator Path (`[ROOT] -> ... -> Class`) | Heap Type | Count"
         " Delta (`Before -> After`) | Self Size Delta (`B`) |"),
        "| :--- | :--- | :--- | :--- |",
    ])
    for row in sorted_paths:
      if row.delta_self_size <= 0 and row.delta_self_count <= 0:
        continue
      lines.append(
          f"| `{row.path}` | `{row.heap_type}` |"
          f" `{_format_signed(row.delta_self_count)}`"
          f" (`{row.before_self_count:,} -> {row.after_self_count:,}`) |"
          f" `{_format_signed(row.delta_self_size)}` |")

  if field_value_diffs:
    regressed_classes = {
        r.class_name
        for r in field_value_diffs
        if r.delta_instance_count > 0 or r.delta_dominated_size > 0
    }
    relevant_fields = [
        r for r in field_value_diffs if r.class_name in regressed_classes
    ]
    if relevant_fields:
      sorted_fields = sorted(
          relevant_fields,
          key=lambda r: (
              r.delta_dominated_size,
              r.delta_instance_count,
              r.after_dominated_size,
          ),
          reverse=True,
      )[:top_n]
      lines.extend([
          "",
          f"## Top {top_n} Regressed Field Values (String & Primitive Fields)",
          "",
          ("| Class Name | Field Name | Field Value | Instance Count Delta"
           " (`Before -> After`) | Dominated Size Delta (`B`) |"),
          "| :--- | :--- | :--- | :--- | :--- |",
      ])
      for row in sorted_fields:
        lines.append(
            f"| `{row.class_name}` | `{row.field_name}` | `{row.field_value}` |"
            f" `{_format_signed(row.delta_instance_count)}`"
            f" (`{row.before_instance_count:,} ->"
            f" {row.after_instance_count:,}`) |"
            f" `{_format_signed(row.delta_dominated_size)}` |")

  if instance_config_diffs:
    regressed_config_classes = {
        r.class_name
        for r in instance_config_diffs
        if r.delta_instance_count > 0 or r.delta_dominated_size > 0
    }
    relevant_configs = [
        r for r in instance_config_diffs
        if r.class_name in regressed_config_classes
    ]
    if relevant_configs:
      sorted_configs = sorted(
          relevant_configs,
          key=lambda r: (
              r.delta_dominated_size,
              r.delta_instance_count,
              r.after_dominated_size,
          ),
          reverse=True,
      )[:top_n]
      lines.extend([
          "",
          (f"## Top {top_n} Regressed Instance Configurations (Field Value"
           " Tuples)"),
          "",
          ("| Class Name | Field Value Tuple (`field=value, ...`) | Instance"
           " Count Delta (`Before -> After`) | Dominated Size Delta (`B`) |"),
          "| :--- | :--- | :--- | :--- |",
      ])
      for row in sorted_configs:
        lines.append(f"| `{row.class_name}` | `{row.config_tuple}` |"
                     f" `{_format_signed(row.delta_instance_count)}`"
                     f" (`{row.before_instance_count:,} ->"
                     f" {row.after_instance_count:,}`) |"
                     f" `{_format_signed(row.delta_dominated_size)}` |")

  return "\n".join(lines) + "\n"


def main() -> None:
  parser = argparse.ArgumentParser(
      description=(
          "Diff two Perfetto / HPROF Java heap dumps at the class and dominator"
          " path levels."))
  parser.add_argument(
      "--before_trace",
      type=Path,
      required=True,
      help="Path to baseline .perfetto-trace or .hprof file.",
  )
  parser.add_argument(
      "--after_trace",
      type=Path,
      required=True,
      help="Path to candidate .perfetto-trace or .hprof file.",
  )
  parser.add_argument(
      "--process_name",
      type=str,
      default=None,
      help="Target Android process name (e.g. com.example.app).",
  )
  parser.add_argument(
      "--class_filter",
      type=str,
      default=None,
      help="Optional substring to filter classes and paths.",
  )
  parser.add_argument(
      "--top_n",
      type=int,
      default=DEFAULT_TOP_N,
      help=f"Number of top rows to display (default: {DEFAULT_TOP_N}).",
  )
  parser.add_argument(
      "--sort_by",
      type=str,
      choices=VALID_SORT_KEYS,
      default=SORT_BY_DOMINATED_SIZE,
      help="Primary metric to sort class regressions by.",
  )
  parser.add_argument(
      "--before_ts",
      type=int,
      default=None,
      help="Optional explicit graph_sample_ts timestamp for baseline dump.",
  )
  parser.add_argument(
      "--after_ts",
      type=int,
      default=None,
      help="Optional explicit graph_sample_ts timestamp for candidate dump.",
  )
  parser.add_argument(
      "--before_index",
      type=int,
      default=None,
      help=(
          "0-based or negative index into sorted heap dumps in before_trace"
          " (e.g. 0 for earliest, -1 for latest). Defaults to 0 when comparing"
          " within a single trace file, else -1."),
  )
  parser.add_argument(
      "--after_index",
      type=int,
      default=None,
      help=("0-based or negative index into sorted heap dumps in after_trace"
            " (e.g. 0 for earliest, -1 for latest). Defaults to -1."),
  )
  parser.add_argument(
      "--output_file",
      type=Path,
      default=None,
      help="Optional path to write Markdown report.",
  )
  parser.add_argument(
      "--trace_processor_bin",
      type=str,
      default=None,
      help="Optional path to trace_processor binary.",
  )
  args = parser.parse_args()

  class_sql = _get_sql_file_path("query_class_aggregation.sql")
  paths_sql = _get_sql_file_path("query_dominator_class_paths.sql")
  field_values_sql = _get_sql_file_path("query_class_field_values.sql")

  tp_bin = args.trace_processor_bin or shutil.which("trace_processor")
  if not tp_bin:
    raise RuntimeError("trace_processor binary not found on PATH; pass"
                       " --trace_processor_bin.")

  with ThreadPoolExecutor(max_workers=2) as executor:
    before_future = executor.submit(
        _run_all_queries_on_trace,
        tp_bin,
        args.before_trace,
        class_sql,
        paths_sql,
        field_values_sql,
    )
    after_future = executor.submit(
        _run_all_queries_on_trace,
        tp_bin,
        args.after_trace,
        class_sql,
        paths_sql,
        field_values_sql,
    )
    before_class_rows, before_path_rows, before_field_rows = (
        before_future.result())
    after_class_rows, after_path_rows, after_field_rows = (
        after_future.result())

  before_classes = parse_class_stats(before_class_rows)
  after_classes = parse_class_stats(after_class_rows)

  same_trace = (
      args.before_trace is not None and args.after_trace is not None and
      args.before_trace.resolve() == args.after_trace.resolve())
  before_idx = (
      args.before_index if args.before_index is not None else
      (0 if same_trace else -1))
  after_idx = args.after_index if args.after_index is not None else -1

  proc_before, before_upid, before_ts, scoped_before_classes = (
      select_process_and_latest_ts(
          before_classes,
          process_name=args.process_name,
          explicit_ts=args.before_ts,
          dump_index=before_idx,
      ))
  proc_after, after_upid, after_ts, scoped_after_classes = (
      select_process_and_latest_ts(
          after_classes,
          process_name=proc_before,
          explicit_ts=args.after_ts,
          dump_index=after_idx,
      ))

  before_available_ts = list_process_dump_timestamps(before_classes,
                                                     proc_before)
  after_available_ts = list_process_dump_timestamps(after_classes, proc_after)

  class_diffs = compute_class_diffs(
      scoped_before_classes,
      scoped_after_classes,
      class_filter=args.class_filter,
  )

  path_diffs: List[DominatorPathDiffRow] = []
  if before_path_rows and after_path_rows:
    before_paths = select_dominator_paths_for_dump(
        parse_dominator_paths(before_path_rows),
        proc_before,
        before_ts,
        upid=before_upid,
    )
    after_paths = select_dominator_paths_for_dump(
        parse_dominator_paths(after_path_rows),
        proc_after,
        after_ts,
        upid=after_upid,
    )
    path_diffs = compute_dominator_path_diffs(
        before_paths, after_paths, class_filter=args.class_filter)

  field_value_diffs: List[FieldValueDiffRow] = []
  instance_config_diffs: List[InstanceConfigDiffRow] = []
  if before_field_rows or after_field_rows:
    before_fields_all, before_configs_all = parse_field_value_and_config_stats(
        before_field_rows)
    after_fields_all, after_configs_all = parse_field_value_and_config_stats(
        after_field_rows)
    before_fields = select_field_values_for_dump(
        before_fields_all, proc_before, before_ts, upid=before_upid)
    after_fields = select_field_values_for_dump(
        after_fields_all, proc_after, after_ts, upid=after_upid)
    field_value_diffs = compute_field_value_diffs(
        before_fields, after_fields, class_filter=args.class_filter)

    before_configs = select_instance_configs_for_dump(
        before_configs_all, proc_before, before_ts, upid=before_upid)
    after_configs = select_instance_configs_for_dump(
        after_configs_all, proc_after, after_ts, upid=after_upid)
    instance_config_diffs = compute_instance_config_diffs(
        before_configs, after_configs, class_filter=args.class_filter)

  report = format_markdown_report(
      process_name=proc_before,
      before_ts=before_ts,
      after_ts=after_ts,
      class_diffs=class_diffs,
      path_diffs=path_diffs,
      top_n=args.top_n,
      sort_by=args.sort_by,
      before_available_ts=before_available_ts,
      after_available_ts=after_available_ts,
      field_value_diffs=field_value_diffs,
      instance_config_diffs=instance_config_diffs,
  )

  sys.stdout.write(report)
  if args.output_file:
    args.output_file.write_text(report, encoding="utf-8")


if __name__ == "__main__":
  main()
