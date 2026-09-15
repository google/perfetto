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
"""Compares two Perfetto traces containing Java or Native heap allocation profiles.

Compares baseline (before) vs candidate (after) Perfetto traces recorded with
heapprofd (Java com.android.art or Native libc.malloc) using
android_heap_profile_summary_tree and heap_profile_allocation. Ranks callstacks
by unreleased size delta (self_size) and total allocation churn delta
(self_alloc_size), as well as per-function cumulative allocation deltas.
"""

import argparse
import csv
from dataclasses import dataclass
import io
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Dict, Iterable, List, Optional, Tuple

DEFAULT_TOP_N = 10
SORT_BY_ALLOC_SIZE = "alloc_size"
SORT_BY_UNRELEASED_SIZE = "unreleased_size"
SORT_BY_ALLOC_COUNT = "alloc_count"
SORT_BY_UNRELEASED_COUNT = "unreleased_count"
VALID_SORT_KEYS = (
    SORT_BY_ALLOC_SIZE,
    SORT_BY_UNRELEASED_SIZE,
    SORT_BY_ALLOC_COUNT,
    SORT_BY_UNRELEASED_COUNT,
)

_COMPILER_SUFFIX_RE = re.compile(r"\s*\(\.__uniq\.\d+\)")

_BOILERPLATE_FRAME_PREFIXES = (
    "ExecuteSwitchImplAsm",
    "void art::interpreter::ExecuteSwitchImplCpp",
    "bool art::interpreter::DoCall",
    "art::interpreter::ArtInterpreterToInterpreterBridge",
    "art::interpreter::Execute",
    "artQuickToInterpreterBridge",
    "art_quick_to_interpreter_bridge",
    "art_quick_invoke_stub",
    "art_quick_invoke_static_stub",
    "art_quick_generic_jni_trampoline",
    "art_jni_trampoline",
    "nterp_helper",
    "art::ArtMethod::Invoke",
    "art::InvokeMethod",
    "_jobject* art::InvokeMethod",
    "art::Method_invoke",
    "art::JNI<true>::CallStaticVoidMethodV",
    "art::JValue art::InvokeWithVarArgs",
    "_JNIEnv::CallStaticVoidMethod",
    "__libc_init",
    "main",
    "android::AndroidRuntime::start",
    "com.android.internal.os.ZygoteInit.main",
    "com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run",
    "android.app.ActivityThread.main",
    "android.os.Looper.loop",
    "android.os.Looper.loopOnce",
    "android.os.Looper.dispatchMessage",
    "android.os.Handler.dispatchMessage",
    "android.app.ActivityThread$H.handleMessage",
    "android.app.ActivityThread.-$$Nest$mhandleReceiver",
    "android.app.ActivityThread.handleReceiver",
    "__start_thread",
    "__pthread_start",
    "art::Thread::CreateCallbackWithUffdGc",
    "art::Thread::CreateCallback",
    "java.lang.Thread.run",
    "java.lang.Daemons$Daemon.run",
    "java.lang.Daemons$ReferenceQueueDaemon.runInternal",
    "java.lang.Daemons$FinalizerDaemon.runInternal",
)


def _strip_compiler_suffix(name: str) -> str:
  return _COMPILER_SUFFIX_RE.sub("", name.strip())


def _is_boilerplate_frame(name: str) -> bool:
  clean = _strip_compiler_suffix(name)
  return any(clean.startswith(prefix) for prefix in _BOILERPLATE_FRAME_PREFIXES)


def _format_concise_callstack_path(path: str) -> str:
  frames = [_strip_compiler_suffix(f) for f in path.split(" -> ") if f.strip()]
  if not frames:
    return path
  filtered: List[str] = []
  had_looper_prefix = False
  for idx, frame in enumerate(frames):
    is_leaf = (idx == len(frames) - 1)
    if _is_boilerplate_frame(frame) and not is_leaf:
      if not filtered:
        had_looper_prefix = True
      continue
    filtered.append(frame)
  if had_looper_prefix and filtered:
    return "[Android Main Looper] -> " + " -> ".join(filtered)
  return " -> ".join(filtered) if filtered else " -> ".join(frames[-5:])


@dataclass(frozen=True)
class CallstackStats:
  """Callstack-level metrics for a single heap allocation profile."""

  process_name: str
  upid: int
  heap_name: str
  path: str
  leaf_function: str
  mapping_name: str
  source_file: str
  line_number: int
  self_size: int
  self_alloc_size: int
  self_count: int
  self_alloc_count: int
  cumulative_size: int
  cumulative_alloc_size: int
  dump_ts: int = 0


@dataclass(frozen=True)
class CallstackDiffRow:
  """Before/after comparison for a single callstack path."""

  path: str
  heap_name: str
  leaf_function: str
  mapping_name: str
  before_self_size: int
  after_self_size: int
  before_self_alloc_size: int
  after_self_alloc_size: int
  before_self_count: int
  after_self_count: int
  before_self_alloc_count: int
  after_self_alloc_count: int

  @property
  def delta_self_size(self) -> int:
    return self.after_self_size - self.before_self_size

  @property
  def delta_self_alloc_size(self) -> int:
    return self.after_self_alloc_size - self.before_self_alloc_size

  @property
  def delta_self_count(self) -> int:
    return self.after_self_count - self.before_self_count

  @property
  def delta_self_alloc_count(self) -> int:
    return self.after_self_alloc_count - self.before_self_alloc_count


@dataclass(frozen=True)
class FunctionSummaryStats:
  """Function-level metrics from android_heap_profile_summary_tree."""

  process_name: str
  upid: int
  heap_name: str
  function_name: str
  mapping_name: str
  self_size: int
  cumulative_size: int
  self_alloc_size: int
  cumulative_alloc_size: int


@dataclass(frozen=True)
class FunctionSummaryDiffRow:
  """Before/after comparison for a single function/method across the profile."""

  function_name: str
  mapping_name: str
  before_self_size: int
  after_self_size: int
  before_cumulative_size: int
  after_cumulative_size: int
  before_self_alloc_size: int
  after_self_alloc_size: int
  before_cumulative_alloc_size: int
  after_cumulative_alloc_size: int

  @property
  def delta_self_size(self) -> int:
    return self.after_self_size - self.before_self_size

  @property
  def delta_cumulative_size(self) -> int:
    return self.after_cumulative_size - self.before_cumulative_size

  @property
  def delta_self_alloc_size(self) -> int:
    return self.after_self_alloc_size - self.before_self_alloc_size

  @property
  def delta_cumulative_alloc_size(self) -> int:
    return self.after_cumulative_alloc_size - self.before_cumulative_alloc_size


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
  return int(float(value))


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


def parse_callstack_stats(
    rows: Iterable[Dict[str, str]],) -> List[CallstackStats]:
  """Parses CSV dictionary rows into CallstackStats objects."""
  parsed: List[CallstackStats] = []
  for row in rows:
    parsed.append(
        CallstackStats(
            process_name=row.get("process_name", ""),
            upid=_parse_int(row.get("upid")),
            heap_name=row.get("heap_name", ""),
            path=row.get("path", ""),
            leaf_function=row.get("leaf_function", ""),
            mapping_name=row.get("mapping_name", ""),
            source_file=row.get("source_file", ""),
            line_number=_parse_int(row.get("line_number")),
            self_size=_parse_int(row.get("self_size")),
            self_alloc_size=_parse_int(row.get("self_alloc_size")),
            self_count=_parse_int(row.get("self_count")),
            self_alloc_count=_parse_int(row.get("self_alloc_count")),
            cumulative_size=_parse_int(row.get("cumulative_size")),
            cumulative_alloc_size=_parse_int(row.get("cumulative_alloc_size")),
            dump_ts=_parse_int(row.get("dump_ts")),
        ))
  return parsed


def parse_function_summary_stats(
    rows: Iterable[Dict[str, str]],) -> List[FunctionSummaryStats]:
  """Parses CSV dictionary rows into FunctionSummaryStats objects."""
  parsed: List[FunctionSummaryStats] = []
  for row in rows:
    parsed.append(
        FunctionSummaryStats(
            process_name=row.get("process_name", ""),
            upid=_parse_int(row.get("upid")),
            heap_name=row.get("heap_name", ""),
            function_name=row.get("function_name", ""),
            mapping_name=row.get("mapping_name", ""),
            self_size=_parse_int(row.get("self_size")),
            cumulative_size=_parse_int(row.get("cumulative_size")),
            self_alloc_size=_parse_int(row.get("self_alloc_size")),
            cumulative_alloc_size=_parse_int(row.get("cumulative_alloc_size")),
        ))
  return parsed


def list_profile_dump_timestamps(
    rows: List[CallstackStats],
    process_name: str,
    heap_name: Optional[str] = None,
) -> List[int]:
  """Returns sorted unique non-zero dump_ts values for a process and optional heap."""
  return sorted({
      r.dump_ts
      for r in rows
      if r.process_name == process_name and r.dump_ts > 0 and
      (not heap_name or heap_name.lower() in r.heap_name.lower())
  })


def select_process_and_heap(
    rows: List[CallstackStats],
    process_name: Optional[str] = None,
    heap_name: Optional[str] = None,
    explicit_ts: Optional[int] = None,
    dump_index: Optional[int] = None,
) -> Tuple[str, str, Optional[int], List[CallstackStats]]:
  """Scopes CallstackStats to a target process, optional heap_name, and optional dump interval."""
  if not rows:
    raise ValueError("No callstack allocation rows found in trace output.")

  available_processes = sorted({r.process_name for r in rows})
  selected_process = process_name or available_processes[0]
  process_rows = [r for r in rows if r.process_name == selected_process]
  if not process_rows and process_name:
    substr_matches = [
        p for p in available_processes if process_name.lower() in p.lower()
    ]
    if len(substr_matches) == 1:
      selected_process = substr_matches[0]
      process_rows = [r for r in rows if r.process_name == selected_process]
  if not process_rows and len(available_processes) == 1:
    selected_process = available_processes[0]
    process_rows = rows
  if not process_rows:
    raise ValueError(
        f"Process '{selected_process}' not found. Available processes:"
        f" {available_processes}")

  available_heaps = sorted({r.heap_name for r in process_rows})
  selected_heap = heap_name or (available_heaps[0]
                                if available_heaps else "all")
  if heap_name:
    scoped_rows = [
        r for r in process_rows if heap_name.lower() in r.heap_name.lower()
    ]
    if not scoped_rows:
      raise ValueError(
          f"Heap '{heap_name}' not found for process '{selected_process}'."
          f" Available heaps: {available_heaps}")
  else:
    scoped_rows = process_rows

  available_ts = sorted({r.dump_ts for r in scoped_rows if r.dump_ts > 0})
  selected_ts: Optional[int] = None
  if explicit_ts is not None:
    selected_ts = explicit_ts
    scoped_rows = [r for r in scoped_rows if r.dump_ts == explicit_ts]
    if not scoped_rows:
      raise ValueError(
          f"No callstack rows found for dump_ts={explicit_ts} in process"
          f" '{selected_process}'. Available dump timestamps: {available_ts}")
  elif dump_index is not None and available_ts:
    try:
      selected_ts = available_ts[dump_index]
    except IndexError as exc:
      raise ValueError(
          f"Profile dump index {dump_index} out of range for process"
          f" '{selected_process}' with {len(available_ts)} available dumps:"
          f" {available_ts}") from exc
    scoped_rows = [r for r in scoped_rows if r.dump_ts == selected_ts]

  return selected_process, selected_heap, selected_ts, scoped_rows


def _aggregate_callstacks(
    rows: Iterable[CallstackStats],) -> Dict[Tuple[str, str], CallstackStats]:
  """Aggregates CallstackStats by (path, heap_name)."""
  aggregated: Dict[Tuple[str, str], CallstackStats] = {}
  for r in rows:
    key = (r.path, r.heap_name)
    existing = aggregated.get(key)
    if existing is None:
      aggregated[key] = r
      continue
    aggregated[key] = CallstackStats(
        process_name=existing.process_name,
        upid=existing.upid,
        heap_name=existing.heap_name,
        path=existing.path,
        leaf_function=existing.leaf_function,
        mapping_name=existing.mapping_name,
        source_file=existing.source_file,
        line_number=existing.line_number,
        self_size=existing.self_size + r.self_size,
        self_alloc_size=existing.self_alloc_size + r.self_alloc_size,
        self_count=existing.self_count + r.self_count,
        self_alloc_count=existing.self_alloc_count + r.self_alloc_count,
        cumulative_size=max(existing.cumulative_size, r.cumulative_size),
        cumulative_alloc_size=max(existing.cumulative_alloc_size,
                                  r.cumulative_alloc_size),
    )
  return aggregated


def compute_callstack_diffs(
    before_rows: List[CallstackStats],
    after_rows: List[CallstackStats],
    function_filter: Optional[str] = None,
) -> List[CallstackDiffRow]:
  """Computes before/after diffs keyed by (path, heap_name)."""
  before_map = _aggregate_callstacks(before_rows)
  after_map = _aggregate_callstacks(after_rows)
  all_keys = sorted(set(before_map.keys()) | set(after_map.keys()))

  diffs: List[CallstackDiffRow] = []
  for path_key, heap_type in all_keys:
    if function_filter and function_filter.lower() not in path_key.lower():
      continue
    b = before_map.get((path_key, heap_type))
    a = after_map.get((path_key, heap_type))
    leaf_func = (a.leaf_function if a else b.leaf_function) if (a or b) else ""
    mapping = (a.mapping_name if a else b.mapping_name) if (a or b) else ""
    diffs.append(
        CallstackDiffRow(
            path=path_key,
            heap_name=heap_type,
            leaf_function=leaf_func,
            mapping_name=mapping,
            before_self_size=b.self_size if b else 0,
            after_self_size=a.self_size if a else 0,
            before_self_alloc_size=b.self_alloc_size if b else 0,
            after_self_alloc_size=a.self_alloc_size if a else 0,
            before_self_count=b.self_count if b else 0,
            after_self_count=a.self_count if a else 0,
            before_self_alloc_count=b.self_alloc_count if b else 0,
            after_self_alloc_count=a.self_alloc_count if a else 0,
        ))
  return diffs


def _aggregate_function_summaries(
    rows: Iterable[FunctionSummaryStats],
) -> Dict[Tuple[str, str], FunctionSummaryStats]:
  """Aggregates FunctionSummaryStats by (function_name, mapping_name)."""
  aggregated: Dict[Tuple[str, str], FunctionSummaryStats] = {}
  for r in rows:
    key = (r.function_name, r.mapping_name)
    existing = aggregated.get(key)
    if existing is None:
      aggregated[key] = r
      continue
    aggregated[key] = FunctionSummaryStats(
        process_name=existing.process_name,
        upid=existing.upid,
        heap_name=existing.heap_name,
        function_name=existing.function_name,
        mapping_name=existing.mapping_name,
        self_size=existing.self_size + r.self_size,
        cumulative_size=max(existing.cumulative_size, r.cumulative_size),
        self_alloc_size=existing.self_alloc_size + r.self_alloc_size,
        cumulative_alloc_size=max(existing.cumulative_alloc_size,
                                  r.cumulative_alloc_size),
    )
  return aggregated


def compute_function_summary_diffs(
    before_rows: List[FunctionSummaryStats],
    after_rows: List[FunctionSummaryStats],
    function_filter: Optional[str] = None,
) -> List[FunctionSummaryDiffRow]:
  """Computes before/after diffs keyed by (function_name, mapping_name)."""
  before_map = _aggregate_function_summaries(before_rows)
  after_map = _aggregate_function_summaries(after_rows)
  all_keys = sorted(set(before_map.keys()) | set(after_map.keys()))

  diffs: List[FunctionSummaryDiffRow] = []
  for func_name, mapping in all_keys:
    if function_filter and function_filter.lower() not in func_name.lower():
      continue
    b = before_map.get((func_name, mapping))
    a = after_map.get((func_name, mapping))
    diffs.append(
        FunctionSummaryDiffRow(
            function_name=func_name,
            mapping_name=mapping,
            before_self_size=b.self_size if b else 0,
            after_self_size=a.self_size if a else 0,
            before_cumulative_size=b.cumulative_size if b else 0,
            after_cumulative_size=a.cumulative_size if a else 0,
            before_self_alloc_size=b.self_alloc_size if b else 0,
            after_self_alloc_size=a.self_alloc_size if a else 0,
            before_cumulative_alloc_size=b.cumulative_alloc_size if b else 0,
            after_cumulative_alloc_size=a.cumulative_alloc_size if a else 0,
        ))
  return diffs


def _format_signed(val: int) -> str:
  return f"+{val:,}" if val > 0 else f"{val:,}"


def format_markdown_report(
    process_name: str,
    heap_name: str,
    callstack_diffs: List[CallstackDiffRow],
    function_diffs: List[FunctionSummaryDiffRow],
    top_n: int,
    sort_by: str,
    before_selected_ts: Optional[int] = None,
    after_selected_ts: Optional[int] = None,
    before_available_ts: Optional[List[int]] = None,
    after_available_ts: Optional[List[int]] = None,
) -> str:
  """Generates a Markdown diff report for Java and Native heap profiles."""
  total_before_unreleased = sum(r.before_self_size for r in callstack_diffs)
  total_after_unreleased = sum(r.after_self_size for r in callstack_diffs)
  delta_unreleased = total_after_unreleased - total_before_unreleased

  total_before_alloc = sum(r.before_self_alloc_size for r in callstack_diffs)
  total_after_alloc = sum(r.after_self_alloc_size for r in callstack_diffs)
  delta_alloc = total_after_alloc - total_before_alloc

  total_before_count = sum(r.before_self_alloc_count for r in callstack_diffs)
  total_after_count = sum(r.after_self_alloc_count for r in callstack_diffs)
  delta_count = total_after_count - total_before_count

  before_window = (f"`ts={before_selected_ts}`" if before_selected_ts
                   is not None else "All profile dumps in trace")
  if before_available_ts and len(before_available_ts) > 1:
    before_window += f" (available dump timestamps: `{before_available_ts}`)"

  after_window = (f"`ts={after_selected_ts}`" if after_selected_ts is not None
                  else "All profile dumps in trace")
  if after_available_ts and len(after_available_ts) > 1:
    after_window += f" (available dump timestamps: `{after_available_ts}`)"

  lines: List[str] = [
      f"# Heap Allocation Profile Diff Report: `{process_name}` (`{heap_name}`)",
      "",
      f"- **Baseline Window (`before`)**: {before_window}",
      f"- **Candidate Window (`after`)**: {after_window}",
      (f"- **Total Allocated Churn (`self_alloc_size`)**:"
       f" `{total_before_alloc:,} B` -> `{total_after_alloc:,} B`"
       f" (`{_format_signed(delta_alloc)} B`)"),
      (f"- **Total Unreleased Heap (`self_size`)**:"
       f" `{total_before_unreleased:,} B` -> `{total_after_unreleased:,} B`"
       f" (`{_format_signed(delta_unreleased)} B`)"),
      (f"- **Total Allocation Count (`self_alloc_count`)**:"
       f" `{total_before_count:,}` -> `{total_after_count:,}`"
       f" (`{_format_signed(delta_count)}`)"),
      "",
  ]

  if function_diffs:
    sorted_functions = sorted(
        function_diffs,
        key=lambda r: (
            0 if _is_boilerplate_frame(r.function_name) else 1,
            r.delta_cumulative_alloc_size,
            r.delta_cumulative_size,
            r.delta_self_alloc_size,
        ),
        reverse=True,
    )[:top_n]
    lines.extend([
        (f"## Top {top_n} Regressed Functions / Methods (Cumulative"
         " Allocation Churn Delta)"),
        "",
        ("| Function / Method Name | Mapping / Library | Cumulative Alloc"
         " Delta (`B`) (`Before -> After`) | Cumulative Unreleased Delta"
         " (`B`) (`Before -> After`) | Self Alloc Delta (`B`) |"),
        "| :--- | :--- | :--- | :--- | :--- |",
    ])
    for row in sorted_functions:
      if (row.delta_cumulative_alloc_size <= 0 and
          row.delta_cumulative_size <= 0):
        continue
      clean_fn = _strip_compiler_suffix(row.function_name)
      lines.append(f"| `{clean_fn}` | `{row.mapping_name}` |"
                   f" `{_format_signed(row.delta_cumulative_alloc_size)}`"
                   f" (`{row.before_cumulative_alloc_size:,} ->"
                   f" {row.after_cumulative_alloc_size:,}`) |"
                   f" `{_format_signed(row.delta_cumulative_size)}`"
                   f" (`{row.before_cumulative_size:,} ->"
                   f" {row.after_cumulative_size:,}`) |"
                   f" `{_format_signed(row.delta_self_alloc_size)}` |")
    lines.append("")

  def _sort_key_for_callstack(row: CallstackDiffRow) -> Tuple[int, int]:
    if sort_by == SORT_BY_UNRELEASED_SIZE:
      return (row.delta_self_size, row.delta_self_alloc_size)
    if sort_by == SORT_BY_ALLOC_COUNT:
      return (row.delta_self_alloc_count, row.delta_self_alloc_size)
    if sort_by == SORT_BY_UNRELEASED_COUNT:
      return (row.delta_self_count, row.delta_self_size)
    return (row.delta_self_alloc_size, row.delta_self_size)

  sorted_by_primary = sorted(
      callstack_diffs, key=_sort_key_for_callstack, reverse=True)[:top_n]

  lines.extend([
      (f"## Top {top_n} Regressed Callstacks (Sorted by `{sort_by}` /"
       " Allocation Churn Delta)"),
      "",
      ("| Callstack Path (`Root -> ... -> Leaf`) | Heap | Leaf Function |"
       " Total Alloc Delta (`B`) (`Before -> After`) | Unreleased Delta (`B`)"
       " (`Before -> After`) | Alloc Count Delta |"),
      "| :--- | :--- | :--- | :--- | :--- | :--- |",
  ])
  for row in sorted_by_primary:
    if row.delta_self_alloc_size <= 0 and row.delta_self_size <= 0:
      continue
    concise_path = _format_concise_callstack_path(row.path)
    clean_leaf = _strip_compiler_suffix(row.leaf_function)
    lines.append(f"| `{concise_path}` | `{row.heap_name}` | `{clean_leaf}` |"
                 f" `{_format_signed(row.delta_self_alloc_size)}`"
                 f" (`{row.before_self_alloc_size:,} ->"
                 f" {row.after_self_alloc_size:,}`) |"
                 f" `{_format_signed(row.delta_self_size)}`"
                 f" (`{row.before_self_size:,} -> {row.after_self_size:,}`) |"
                 f" `{_format_signed(row.delta_self_alloc_count)}` |")

  if sort_by != SORT_BY_UNRELEASED_SIZE and "art" not in heap_name.lower():
    sorted_by_unreleased = sorted(
        callstack_diffs,
        key=lambda r: (r.delta_self_size, r.delta_self_alloc_size),
        reverse=True,
    )[:top_n]
    lines.extend([
        "",
        (f"## Top {top_n} Regressed Callstacks by Unreleased Size Delta"
         " (`self_size`)"),
        "",
        ("| Callstack Path (`Root -> ... -> Leaf`) | Heap | Leaf Function |"
         " Unreleased Delta (`B`) (`Before -> After`) | Total Alloc Delta"
         " (`B`) (`Before -> After`) | Unreleased Count Delta |"),
        "| :--- | :--- | :--- | :--- | :--- | :--- |",
    ])
    for row in sorted_by_unreleased:
      if row.delta_self_size <= 0:
        continue
      concise_path = _format_concise_callstack_path(row.path)
      clean_leaf = _strip_compiler_suffix(row.leaf_function)
      lines.append(f"| `{concise_path}` | `{row.heap_name}` | `{clean_leaf}` |"
                   f" `{_format_signed(row.delta_self_size)}`"
                   f" (`{row.before_self_size:,} -> {row.after_self_size:,}`) |"
                   f" `{_format_signed(row.delta_self_alloc_size)}`"
                   f" (`{row.before_self_alloc_size:,} ->"
                   f" {row.after_self_alloc_size:,}`) |"
                   f" `{_format_signed(row.delta_self_count)}` |")

  return "\n".join(lines) + "\n"


def main() -> None:
  parser = argparse.ArgumentParser(
      description=(
          "Diff two Perfetto Java (com.android.art) or Native (libc.malloc)"
          " heap allocation profiles."))
  parser.add_argument(
      "--before_trace",
      type=Path,
      required=True,
      help="Path to baseline .perfetto-trace file.",
  )
  parser.add_argument(
      "--after_trace",
      type=Path,
      required=True,
      help="Path to candidate .perfetto-trace file.",
  )
  parser.add_argument(
      "--process_name",
      type=str,
      default=None,
      help="Target Android process name (e.g. com.android.systemui).",
  )
  parser.add_argument(
      "--heap_name",
      type=str,
      default=None,
      help="Optional heap filter (e.g. com.android.art or libc.malloc).",
  )
  parser.add_argument(
      "--function_filter",
      type=str,
      default=None,
      help="Optional substring to filter callstacks and function names.",
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
      default=SORT_BY_ALLOC_SIZE,
      help="Primary metric to sort callstack regressions by.",
  )
  parser.add_argument(
      "--before_ts",
      type=int,
      default=None,
      help="Optional explicit dump timestamp (ts) for baseline profile slice.",
  )
  parser.add_argument(
      "--after_ts",
      type=int,
      default=None,
      help="Optional explicit dump timestamp (ts) for candidate profile slice.",
  )
  parser.add_argument(
      "--before_index",
      type=int,
      default=None,
      help=(
          "0-based or negative index into continuous dump slices in"
          " before_trace (e.g. 0 for first slice, -1 for last slice). Defaults"
          " to 0 when comparing within a single trace file, else all slices."),
  )
  parser.add_argument(
      "--after_index",
      type=int,
      default=None,
      help=(
          "0-based or negative index into continuous dump slices in after_trace"
          " (e.g. 0 for first slice, -1 for last slice). Defaults to -1 when"
          " comparing within a single trace file, else all slices."),
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

  callstacks_sql = _get_sql_file_path("query_heap_profile_callstacks.sql")
  summary_sql = _get_sql_file_path("query_heap_profile_summary_tree.sql")

  tp_bin = args.trace_processor_bin or shutil.which("trace_processor")
  if not tp_bin:
    raise RuntimeError("trace_processor binary not found on PATH; pass"
                       " --trace_processor_bin.")
  before_cs_rows = _run_trace_processor_query(tp_bin, args.before_trace,
                                              callstacks_sql)
  after_cs_rows = _run_trace_processor_query(tp_bin, args.after_trace,
                                             callstacks_sql)
  before_sum_rows = _run_trace_processor_query(tp_bin, args.before_trace,
                                               summary_sql)
  after_sum_rows = _run_trace_processor_query(tp_bin, args.after_trace,
                                              summary_sql)

  before_callstacks = parse_callstack_stats(before_cs_rows)
  after_callstacks = parse_callstack_stats(after_cs_rows)

  same_trace = (
      args.before_trace is not None and args.after_trace is not None and
      args.before_trace.resolve() == args.after_trace.resolve())
  before_idx = (
      args.before_index if args.before_index is not None else
      (0 if same_trace else None))
  after_idx = (
      args.after_index if args.after_index is not None else
      (-1 if same_trace else None))

  proc_before, heap_before, before_sel_ts, scoped_before_cs = (
      select_process_and_heap(
          before_callstacks,
          process_name=args.process_name,
          heap_name=args.heap_name,
          explicit_ts=args.before_ts,
          dump_index=before_idx,
      ))
  proc_after, _, after_sel_ts, scoped_after_cs = select_process_and_heap(
      after_callstacks,
      process_name=proc_before,
      heap_name=args.heap_name,
      explicit_ts=args.after_ts,
      dump_index=after_idx,
  )

  before_available_ts = list_profile_dump_timestamps(before_callstacks,
                                                     proc_before,
                                                     args.heap_name)
  after_available_ts = list_profile_dump_timestamps(after_callstacks,
                                                    proc_after, args.heap_name)

  callstack_diffs = compute_callstack_diffs(
      scoped_before_cs,
      scoped_after_cs,
      function_filter=args.function_filter,
  )

  function_diffs: List[FunctionSummaryDiffRow] = []
  if before_sum_rows and after_sum_rows:
    before_funcs = parse_function_summary_stats(before_sum_rows)
    after_funcs = parse_function_summary_stats(after_sum_rows)
    function_diffs = compute_function_summary_diffs(
        before_funcs,
        after_funcs,
        function_filter=args.function_filter,
    )

  report = format_markdown_report(
      process_name=proc_before,
      heap_name=heap_before,
      callstack_diffs=callstack_diffs,
      function_diffs=function_diffs,
      top_n=args.top_n,
      sort_by=args.sort_by,
      before_selected_ts=before_sel_ts,
      after_selected_ts=after_sel_ts,
      before_available_ts=before_available_ts,
      after_available_ts=after_available_ts,
  )

  sys.stdout.write(report)
  if args.output_file:
    args.output_file.write_text(report, encoding="utf-8")


if __name__ == "__main__":
  main()
