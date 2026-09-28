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
"""Helpers shared by the Bigtrace server modules."""

from datetime import datetime, timezone
import hashlib
import logging
import os
from typing import Any, Dict, Iterable, List, Optional
import uuid

logger = logging.getLogger(__name__)

MANIFEST_NAME = "bigtrace_manifest.json"

IGNORED_DIRS = {
    ".git",
    ".hg",
    ".svn",
    ".venv",
    "venv",
    "__pycache__",
    "node_modules",
    "buildtools",
    "out",
}


def column_schema(name: str, display_name: str, col_type: str,
                  default_visible: bool, description: str) -> Dict[str, Any]:
  return {
      "name": name,
      "displayName": display_name,
      "type": col_type,
      "defaultVisible": default_visible,
      "description": description,
  }


def iso_now() -> str:
  return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def iso_time(epoch_secs: float) -> str:
  return datetime.fromtimestamp(
      epoch_secs, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def generate_trace_uuid(path: str) -> str:
  """Returns a UUID derived from the absolute path, stable across restarts."""
  digest = hashlib.md5(os.path.abspath(path).encode("utf-8")).hexdigest()
  return str(uuid.UUID(digest))


def is_manifest_file(file_name: str) -> bool:
  base = os.path.basename(file_name).lower()
  return base == "manifest.json" or base.endswith(
      ("_manifest.json", "_manifest.csv"))


def as_int(value: Any, default: Optional[int]) -> Optional[int]:
  try:
    return int(value)
  except (TypeError, ValueError):
    return default


def as_bool(value: Any) -> bool:
  if isinstance(value, str):
    return value.strip().lower() in ("1", "true", "yes")
  return bool(value)


def get_param(body: Dict[str, Any],
              snake_name: str,
              default: Any = None) -> Any:
  """Reads a request field sent either as snake_case or camelCase."""
  value = body.get(snake_name)
  if value is None:
    head, *rest = snake_name.split("_")
    value = body.get(head + "".join(w.title() for w in rest))
  return default if value is None else value


def to_wire(value: Any) -> Optional[str]:
  """Formats a cell the way the UI expects: strings, or null."""
  if value is None:
    return None
  if isinstance(value, bool):
    return "true" if value else "false"
  return str(value)


def infer_column_type(values: Iterable[Any]) -> str:
  """Infers a Bigtrace column type from the first non-empty value."""
  sample = next((v for v in values if v not in (None, "", "NULL")), None)
  if sample is None:
    return "STRING"
  if isinstance(sample, bool):
    return "BOOL"
  if isinstance(sample, int):
    return "INT64"
  if isinstance(sample, float):
    return "FLOAT64"
  if isinstance(sample, bytes):
    return "BYTES"
  text = str(sample)
  if text.lower() in ("true", "false"):
    return "BOOL"
  for parse, col_type in ((int, "INT64"), (float, "FLOAT64")):
    try:
      parse(text)
      return col_type
    except ValueError:
      pass
  return "STRING"


def default_worker_count() -> int:
  return min(32, os.cpu_count() or 4)


def resolve_trace_processor_bin(
    bin_path: Optional[str] = None) -> Optional[str]:
  """Finds trace_processor_shell: flag, then env vars, then local builds.

  Returns None to let TraceProcessor download a prebuilt binary.
  """
  if bin_path:
    return bin_path
  repo_root = os.path.abspath(
      os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
  candidates = [
      os.environ.get("SHELL_PATH"),
      os.environ.get("TRACE_PROCESSOR_BIN"),
  ] + [
      os.path.join(repo_root, "out", d, "trace_processor_shell")
      for d in ("release", "linux", "default")
  ]
  for cand in candidates:
    if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
      return cand
  return None


def find_trace_files(root: str, recursive: bool) -> List[str]:
  """Lists candidate trace files. Anything may be a trace, whatever its name.

  Hidden files and manifests are skipped. A non-recursive scan of a directory
  that only has subdirectories falls back to a recursive one.
  """
  if not os.path.isdir(root):
    logger.warning("Trace directory does not exist: %s", root)
    return []

  def is_candidate(path: str) -> bool:
    name = os.path.basename(path)
    return not name.startswith(".") and not is_manifest_file(name)

  def walk() -> Iterable[str]:
    for dir_path, subdirs, files in os.walk(root):
      subdirs[:] = [
          d for d in subdirs if d not in IGNORED_DIRS and not d.startswith(".")
      ]
      for f in files:
        yield os.path.join(dir_path, f)

  if recursive:
    return sorted(p for p in walk() if is_candidate(p))
  top_level = [
      p for p in (os.path.join(root, f) for f in os.listdir(root))
      if os.path.isfile(p) and is_candidate(p)
  ]
  return sorted(top_level or (p for p in walk() if is_candidate(p)))
