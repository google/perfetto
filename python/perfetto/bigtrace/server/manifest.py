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
"""Generates a manifest of per-trace metadata extracted in parallel."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import logging
import os
from typing import Any, Dict, List, Optional

from perfetto.trace_processor.api import (
    TraceProcessorConfig,
    TraceProcessorException,
)
from perfetto.bigtrace.server.catalog import DEFAULT_PRESETS
from perfetto.bigtrace.server.trace_processor_pool import (
    close_quietly,
    load_trace,
)
from perfetto.bigtrace.server.util import (
    MANIFEST_NAME,
    column_schema,
    default_worker_count,
    find_trace_files,
    generate_trace_uuid,
    infer_column_type,
    iso_time,
    resolve_trace_processor_bin,
)

logger = logging.getLogger(__name__)

# Columns the manifest generator extracts, in display order.
MANIFEST_COLUMNS = [
    column_schema("file_name", "File Name", "STRING", True, "Trace file name"),
    column_schema("trace_uuid", "Trace UUID", "STRING", True,
                  "Trace identifier"),
    column_schema("duration_ms", "Duration (ms)", "FLOAT64", True,
                  "Trace duration in milliseconds"),
    column_schema("slice_count", "Slice Count", "INT64", True,
                  "Number of slices"),
    column_schema("android_build_fingerprint", "Android Build", "STRING", True,
                  "Android build fingerprint"),
    column_schema("android_sdk_version", "Android SDK", "INT64", True,
                  "Android SDK version"),
    column_schema("process_count", "Process Count", "INT64", False,
                  "Number of processes"),
    column_schema("thread_count", "Thread Count", "INT64", False,
                  "Number of threads"),
    column_schema("system_machine", "Architecture", "STRING", False,
                  "CPU architecture"),
    column_schema("system_name", "OS Name", "STRING", False,
                  "Operating system"),
    column_schema("file_size", "File Size (bytes)", "INT64", False,
                  "File size in bytes"),
    column_schema("start_ts", "Start Timestamp (ns)", "INT64", False,
                  "Trace start timestamp"),
    column_schema("end_ts", "End Timestamp (ns)", "INT64", False,
                  "Trace end timestamp"),
    column_schema("file_path", "File Path", "STRING", False,
                  "Path to the trace"),
    column_schema("modified_time", "Modified Time", "TIMESTAMP", False,
                  "File modification time"),
]

# Rows of the trace `metadata` table copied into the manifest.
MANIFEST_METADATA_KEYS = {
    "android_build_fingerprint",
    "android_incremental_build",
    "android_sdk_version",
    "benchmark_name",
    "story_name",
    "system_machine",
    "system_name",
    "system_release",
    "system_version",
    "trace_type",
}


class BigtraceManifestGenerator:
  """Builds a manifest by extracting metadata from every trace in parallel.

  Traces whose size and mtime match an entry of the existing manifest are
  reused instead of being loaded again.
  """

  def __init__(
      self,
      trace_dir: str,
      recursive: bool = False,
      max_workers: Optional[int] = None,
      bin_path: Optional[str] = None,
      metadata_query: Optional[str] = None,
  ):
    self.trace_dir = os.path.abspath(trace_dir)
    self.recursive = recursive
    self.max_workers = max_workers or default_worker_count()
    self.tp_config = TraceProcessorConfig(
        bin_path=resolve_trace_processor_bin(bin_path))
    self.metadata_query = metadata_query

  def _extract(self, path: str) -> Dict[str, Any]:
    """Returns the manifest record for one trace. Raises for non-traces."""
    st = os.stat(path)
    tp = load_trace(path, self.tp_config)
    try:
      meta: Dict[str, Any] = {}
      bounds = next(
          iter(tp.query("SELECT start_ts, end_ts FROM trace_bounds")), None)
      if bounds is not None and bounds.start_ts is not None:
        meta["start_ts"] = bounds.start_ts
        meta["end_ts"] = bounds.end_ts
        meta["duration_ms"] = round((bounds.end_ts - bounds.start_ts) / 1e6, 3)
      counts = next(
          iter(
              tp.query("SELECT (SELECT count(1) FROM process) AS p, "
                       "(SELECT count(1) FROM thread) AS t, "
                       "(SELECT count(1) FROM slice) AS s")))
      meta.update(
          process_count=counts.p, thread_count=counts.t, slice_count=counts.s)
      trace_uuid = None
      for r in tp.query("SELECT name, int_value, str_value FROM metadata"):
        value = r.int_value if r.int_value is not None else r.str_value
        if value is None:
          continue
        if r.name == "trace_uuid":
          trace_uuid = str(value)
        elif r.name in MANIFEST_METADATA_KEYS:
          meta[r.name] = value
      if self.metadata_query:
        try:
          result = tp.query(self.metadata_query)
          first = next(iter(result), None)
          for col in result.column_names if first is not None else []:
            if getattr(first, col) is not None:
              meta[col] = getattr(first, col)
        except TraceProcessorException as e:
          logger.warning("Metadata query failed on %s: %s", path, e)
    finally:
      close_quietly(tp)
    return {
        "file_name": os.path.basename(path),
        "file_path": os.path.relpath(path, self.trace_dir),
        "trace_uuid": trace_uuid or generate_trace_uuid(path),
        "file_size": st.st_size,
        "modified_time": iso_time(st.st_mtime),
        **meta,
    }

  @staticmethod
  def _build_schema(traces: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    keys = {k for t in traces for k in t}
    schema = [c for c in MANIFEST_COLUMNS if c["name"] in keys]
    known = {c["name"] for c in MANIFEST_COLUMNS}
    for key in sorted(keys - known):
      display_name = key.replace("_", " ").title()
      col_type = infer_column_type(t.get(key) for t in traces)
      schema.append(
          column_schema(key, display_name, col_type, True,
                        f"Metadata: {display_name}"))
    return schema

  @staticmethod
  def _read_manifest(path: str) -> Dict[str, Any]:
    if not os.path.isfile(path):
      return {}
    try:
      with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
      return data if isinstance(data, dict) else {}
    except (OSError, ValueError) as e:
      logger.warning("Ignoring unreadable manifest %s: %s", path, e)
      return {}

  def generate(
      self,
      output_path: Optional[str] = None,
      existing_manifest_path: Optional[str] = None,
      force: bool = False,
  ) -> Dict[str, Any]:
    """Builds the manifest, writes it to `output_path` if set, returns it."""
    previous = {} if force else self._read_manifest(
        existing_manifest_path or output_path or
        os.path.join(self.trace_dir, MANIFEST_NAME))
    cached = {
        os.path.normpath(t.get("file_path") or t.get("path")): t
        for t in previous.get("traces", [])
        if isinstance(t, dict) and (t.get("file_path") or t.get("path"))
    }

    traces: List[Dict[str, Any]] = []
    to_extract: List[str] = []
    for path in find_trace_files(self.trace_dir, self.recursive):
      try:
        st = os.stat(path)
      except OSError as e:
        logger.warning("Cannot stat %s: %s", path, e)
        continue
      if st.st_size == 0:
        continue
      old = cached.get(os.path.normpath(os.path.relpath(path, self.trace_dir)))
      if old and old.get("file_size") == st.st_size and old.get(
          "modified_time") in (None, iso_time(st.st_mtime)):
        traces.append(old)
      else:
        to_extract.append(path)

    logger.info("%d trace(s) cached, %d to extract with %d workers",
                len(traces), len(to_extract), self.max_workers)
    with ThreadPoolExecutor(max_workers=self.max_workers) as pool:
      futures = {pool.submit(self._extract, p): p for p in to_extract}
      for done, future in enumerate(as_completed(futures), 1):
        name = os.path.basename(futures[future])
        try:
          traces.append(future.result())
          logger.info("[%d/%d] Extracted %s", done, len(to_extract), name)
        except Exception as e:  # pylint: disable=broad-except
          logger.info("[%d/%d] Skipped %s: %s", done, len(to_extract), name, e)

    traces.sort(key=lambda t: t.get("file_path", ""))
    manifest = {
        "trace_dir": ".",
        "schema": self._build_schema(traces),
        "traces": traces,
        "presets": previous.get("presets", DEFAULT_PRESETS),
        "experiments": previous.get("experiments", []),
    }
    if output_path:
      out = os.path.abspath(output_path)
      os.makedirs(os.path.dirname(out), exist_ok=True)
      # Write then rename so a running server never reads a partial file.
      tmp = out + ".tmp"
      with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
      os.replace(tmp, out)
      logger.info("Wrote %d traces to %s", len(traces), out)
    return manifest


def generate_manifest(trace_dir: str,
                      output_path: Optional[str] = None,
                      existing_manifest_path: Optional[str] = None,
                      recursive: bool = False,
                      max_workers: Optional[int] = None,
                      bin_path: Optional[str] = None,
                      metadata_query: Optional[str] = None,
                      force: bool = False) -> Dict[str, Any]:
  return BigtraceManifestGenerator(
      trace_dir=trace_dir,
      recursive=recursive,
      max_workers=max_workers,
      bin_path=bin_path,
      metadata_query=metadata_query,
  ).generate(
      output_path=output_path,
      existing_manifest_path=existing_manifest_path,
      force=force)
