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
"""The set of traces the server queries, loaded from a directory or manifest."""

import csv
import dataclasses as dc
import json
import logging
import os
import threading
from typing import Any, Dict, List, Optional, Tuple
import urllib.parse

from perfetto.bigtrace.server.filters import (
    evaluate_filters,
    parse_experiment_filter,
    parse_order_by,
    sort_records,
)
from perfetto.bigtrace.server.util import (
    MANIFEST_NAME,
    as_bool,
    as_int,
    column_schema,
    find_trace_files,
    generate_trace_uuid,
    get_param,
    infer_column_type,
    iso_time,
)

logger = logging.getLogger(__name__)

DEFAULT_UI_ORIGIN = "https://ui.perfetto.dev"

# Columns every catalog has, whatever the trace source.
CATALOG_COLUMNS = [
    column_schema("link", "Link", "STRING", True,
                  "Opens the trace in the Perfetto UI"),
    column_schema("file_name", "File Name", "STRING", True, "Trace file name"),
    column_schema("file_size", "File Size", "INT64", True,
                  "File size in bytes"),
    column_schema("trace_uuid", "Trace UUID", "STRING", True,
                  "Trace identifier"),
    column_schema("file_path", "File Path", "STRING", False,
                  "Path to the trace"),
    column_schema("modified_time", "Modified Time", "TIMESTAMP", False,
                  "File modification time"),
]

DEFAULT_PRESETS = [
    {
        "id": "builtin:slice_count",
        "category": "General",
        "name": "Slice count per trace",
        "description": "Count total slices in each trace",
        "perfettoSql": "SELECT count(1) AS slice_count FROM slice;",
        "icon": "analytics",
    },
    {
        "id": "builtin:slow_slices",
        "category": "Performance",
        "name": "Slow slices (>10ms)",
        "description": "Find slices lasting longer than 10 milliseconds",
        "perfettoSql": ("SELECT name, dur / 1e6 AS dur_ms FROM slice "
                        "WHERE dur > 10000000 ORDER BY dur DESC;"),
        "icon": "timer",
    },
    {
        "id": "builtin:threads_summary",
        "category": "Processes",
        "name": "Threads per process",
        "description": "Summary of thread counts per process",
        "perfettoSql": ("SELECT process.name AS process_name, "
                        "count(thread.utid) AS thread_count FROM process "
                        "JOIN thread USING (upid) GROUP BY process.name "
                        "ORDER BY thread_count DESC;"),
        "icon": "memory",
    },
]

# TraceEntry attributes; any other key is looked up in its metadata.
_ENTRY_ATTRS = {
    "trace_uuid", "file_path", "file_name", "file_size", "modified_time", "link"
}


@dc.dataclass
class TraceEntry:
  trace_uuid: str
  file_path: str
  file_name: str
  file_size: int
  modified_time: Optional[str]
  link: str
  metadata: Dict[str, Any] = dc.field(default_factory=dict)

  def get_value(self, key: str) -> Any:
    if key in _ENTRY_ATTRS:
      return getattr(self, key)
    return self.metadata.get(key)


@dc.dataclass
class SettingsOverrides:
  """Trace source settings the UI sends with each request."""
  trace_dir: Optional[str] = None
  recursive: Optional[bool] = None
  manifest_path: Optional[str] = None
  trace_uuids: Optional[List[str]] = None

  @staticmethod
  def parse(settings: Any) -> "SettingsOverrides":
    """Parses the UI's settings list. Empty values mean "not overridden"."""
    out = SettingsOverrides()
    if not isinstance(settings, list):
      return out
    for s in settings:
      if not isinstance(s, dict):
        continue
      values = [str(v).strip() for v in s.get("values") or []]
      values = [v for v in values if v]
      if not values:
        continue
      setting_id = get_param(s, "setting_id")
      if setting_id == "trace_directory":
        out.trace_dir = values[0]
      elif setting_id == "recursive":
        out.recursive = as_bool(values[0])
      elif setting_id == "trace_manifest":
        out.manifest_path = values[0]
      elif setting_id == "trace_uuids":
        out.trace_uuids = values
    return out


class TraceCatalog:
  """The set of traces queries run over, with their metadata."""

  def __init__(self,
               trace_dir: Optional[str] = None,
               recursive: bool = False,
               manifest_path: Optional[str] = None,
               base_url: str = "",
               ui_origin: str = DEFAULT_UI_ORIGIN):
    self.trace_dir = os.path.abspath(trace_dir or os.getcwd())
    self.recursive = recursive
    self.manifest_path = os.path.abspath(
        manifest_path) if manifest_path else None
    self.base_url = base_url.rstrip("/")
    self.ui_origin = ui_origin.rstrip("/")
    self._traces: List[TraceEntry] = []
    self._experiments: List[Dict[str, Any]] = []
    self._presets: List[Dict[str, Any]] = []
    self._manifest_schema: List[Dict[str, Any]] = []
    self._lock = threading.RLock()
    self.reload()

  def is_configured_with(self,
                         trace_dir: Optional[str] = None,
                         recursive: Optional[bool] = None,
                         manifest_path: Optional[str] = None) -> bool:
    with self._lock:
      return ((not trace_dir or os.path.abspath(trace_dir) == self.trace_dir)
              and (recursive is None or recursive == self.recursive) and
              (not manifest_path or
               os.path.abspath(manifest_path) == self.manifest_path))

  def apply_overrides(self, overrides: SettingsOverrides) -> None:
    """Reloads if the request points at a different trace source."""
    with self._lock:
      if not self.is_configured_with(overrides.trace_dir, overrides.recursive,
                                     overrides.manifest_path):
        self.reload(overrides.trace_dir, overrides.recursive,
                    overrides.manifest_path)

  def reload(self,
             trace_dir: Optional[str] = None,
             recursive: Optional[bool] = None,
             manifest_path: Optional[str] = None) -> None:
    """Re-reads the source. Empty arguments keep the current configuration."""
    with self._lock:
      if trace_dir and os.path.abspath(trace_dir) != self.trace_dir:
        self.trace_dir = os.path.abspath(trace_dir)
        self.manifest_path = None
      if recursive is not None:
        self.recursive = bool(recursive)
      if manifest_path:
        self.manifest_path = os.path.abspath(manifest_path)
      if not self.manifest_path:
        candidate = os.path.join(self.trace_dir, MANIFEST_NAME)
        if os.path.isfile(candidate):
          self.manifest_path = candidate

      self._experiments, self._presets, self._manifest_schema = [], [], []
      path = self.manifest_path
      if path and os.path.isfile(path):
        is_csv = path.lower().endswith(".csv")
        self._traces = (
            self._load_csv(path) if is_csv else self._load_manifest(path))
      else:
        self._traces = [
            self._make_entry({"file_path": p}, self.trace_dir)
            for p in find_trace_files(self.trace_dir, self.recursive)
        ]

  def _make_entry(self, record: Dict[str, Any], base_dir: str) -> TraceEntry:
    path = record.get("file_path") or record.get("path")
    if not os.path.isabs(path):
      candidate = os.path.normpath(os.path.join(base_dir, path))
      fallback = os.path.normpath(os.path.join(self.trace_dir, path))
      use_fallback = not os.path.exists(candidate) and os.path.exists(fallback)
      path = fallback if use_fallback else candidate
    try:
      st = os.stat(path)
      size, mtime = st.st_size, iso_time(st.st_mtime)
    except OSError:
      size, mtime = 0, record.get("modified_time")
    file_name = record.get("file_name") or os.path.basename(path)
    trace_uuid = record.get("trace_uuid") or generate_trace_uuid(path)
    # The Perfetto UI fetches ?url= traces served from localhost. The URL has
    # no query string, which the UI's service worker would reject.
    trace_url = (f"{self.base_url}/trace/{trace_uuid}/"
                 f"{urllib.parse.quote(file_name)}")
    link = record.get("link") or (
        f"{self.ui_origin}/#!/?url={trace_url}&referrer=bigtrace_server")
    return TraceEntry(
        trace_uuid=str(trace_uuid),
        file_path=path,
        file_name=file_name,
        file_size=as_int(record.get("file_size"), size),
        modified_time=mtime,
        link=link,
        metadata={
            k: v
            for k, v in record.items()
            if k not in _ENTRY_ATTRS and k != "path"
        },
    )

  def _load_manifest(self, manifest_file: str) -> List[TraceEntry]:
    try:
      with open(manifest_file, "r", encoding="utf-8") as f:
        data = json.load(f)
    except (OSError, ValueError) as e:
      logger.error("Failed to parse manifest %s: %s", manifest_file, e)
      return []
    base_dir = os.path.dirname(manifest_file)
    records = data
    if isinstance(data, dict):
      records = data.get("traces", [])
      self._experiments = data.get("experiments") or []
      self._presets = data.get("presets") or []
      self._manifest_schema = data.get("schema") or []
      base_dir = os.path.normpath(
          os.path.join(base_dir, data.get("trace_dir", ".")))
    if not isinstance(records, list):
      return []
    return [
        self._make_entry(r, base_dir)
        for r in records
        if isinstance(r, dict) and (r.get("file_path") or r.get("path"))
    ]

  def _load_csv(self, csv_file: str) -> List[TraceEntry]:
    try:
      with open(csv_file, "r", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    except (OSError, csv.Error) as e:
      logger.error("Failed to load CSV %s: %s", csv_file, e)
      return []
    base_dir = os.path.dirname(csv_file)
    return [
        self._make_entry(r, base_dir)
        for r in rows
        if r.get("file_path") or r.get("path")
    ]

  def get_traces(self) -> List[TraceEntry]:
    with self._lock:
      return list(self._traces)

  def get_experiments(self) -> List[Dict[str, Any]]:
    with self._lock:
      return list(self._experiments)

  def get_presets(self) -> List[Dict[str, Any]]:
    with self._lock:
      return list(self._presets)

  def get_schema(self) -> List[Dict[str, Any]]:
    """Returns manifest columns, then built-in ones, then discovered keys."""
    with self._lock:
      traces = list(self._traces)
      manifest_schema = list(self._manifest_schema)
    cols: Dict[str, Dict[str, Any]] = {}
    for c in manifest_schema + CATALOG_COLUMNS:
      if isinstance(c, dict) and "name" in c:
        cols.setdefault(c["name"], dict(c))
    for k in sorted({k for t in traces for k in t.metadata}):
      if k not in cols:
        col_type = infer_column_type(t.metadata.get(k) for t in traces)
        cols[k] = column_schema(k,
                                k.replace("_", " ").title(), col_type, True,
                                f"Metadata: {k}")
    return list(cols.values())

  def filter_and_sort(
      self,
      filters: Optional[List[Dict[str, Any]]] = None,
      order_by: Optional[str] = None,
      experiment_filter: Optional[Dict[str, Any]] = None,
      trace_uuids: Optional[List[str]] = None,
      limit: Optional[int] = None,
      offset: int = 0,
  ) -> Tuple[List[TraceEntry], int]:
    """Returns (page of traces, number of traces matching before paging).

    A missing or non-positive limit means no limit.
    """
    traces = self.get_traces()
    if trace_uuids:
      wanted = set(trace_uuids)
      traces = [
          t for t in traces if t.trace_uuid in wanted or t.file_name in wanted
      ]
    experiment = parse_experiment_filter(experiment_filter)
    if experiment:
      traces = [t for t in traces if self._in_experiment_arm(t, experiment)]
    if filters:
      traces = [t for t in traces if evaluate_filters(t.get_value, filters)]
    total = len(traces)
    if order_by:
      traces = sort_records(traces, lambda t, col: t.get_value(col),
                            parse_order_by(order_by))
    offset = max(0, offset)
    end = offset + limit if limit and limit > 0 else None
    return traces[offset:end], total

  @staticmethod
  def _in_experiment_arm(trace: TraceEntry, experiment: Dict[str, Any]) -> bool:
    if (as_int(trace.get_value("experiment_id"),
               None) != experiment["experimentId"] or
        as_int(trace.get_value("control_id"), None) != experiment["controlId"]):
      return False
    arm = trace.get_value("is_treatment")
    return arm is None or as_bool(arm) == experiment["isTreatment"]
