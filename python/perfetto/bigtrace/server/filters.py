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
"""DataGrid-style filtering and AIP-132 sorting of records."""

import fnmatch
from typing import Any, Callable, Dict, List, Optional, Tuple

from perfetto.bigtrace.server.util import as_bool, as_int, get_param


def is_null(value: Any) -> bool:
  return value is None or value == "" or (isinstance(value, str) and
                                          value.upper() == "NULL")


def _equals(value: Any, target: Any) -> bool:
  if str(value).lower() == str(target).lower():
    return True
  try:
    return float(value) == float(target)
  except (TypeError, ValueError):
    return False


def _compare(value: Any, target: Any) -> int:
  try:
    a, b = float(value), float(target)
  except (TypeError, ValueError):
    a, b = str(value), str(target)
  return (a > b) - (a < b)


_COMPARISONS = {
    "<": lambda c: c < 0,
    "<=": lambda c: c <= 0,
    ">": lambda c: c > 0,
    ">=": lambda c: c >= 0,
}

_FILTER_OPS = {
    "=", "!=", "<", "<=", ">", ">=", "glob", "not glob", "in", "not in",
    "is null", "is not null"
}


def evaluate_filter_op(value: Any, op: str, target: Any) -> bool:
  """Evaluates one DataGrid filter operator against a value."""
  op = op.lower()
  if op not in _FILTER_OPS:
    raise ValueError(f"Unsupported filter op: {op}")
  if op == "is null":
    return is_null(value)
  if op == "is not null":
    return not is_null(value)
  if value is None:
    return False
  if op == "=":
    return _equals(value, target)
  if op == "!=":
    return not _equals(value, target)
  if op in _COMPARISONS:
    return _COMPARISONS[op](_compare(value, target))
  if op in ("glob", "not glob"):
    pattern = "" if target is None else str(target)
    return fnmatch.fnmatchcase(str(value), pattern) == (op == "glob")
  targets = target if isinstance(target, (list, tuple)) else [target]
  return any(_equals(value, t) for t in targets) == (op == "in")


def evaluate_filters(get_value: Callable[[str], Any],
                     filters: Optional[List[Dict[str, Any]]]) -> bool:
  return all(
      evaluate_filter_op(
          get_value(f["field"]), f.get("op", "="), f.get("value"))
      for f in filters or []
      if f.get("field"))


def parse_order_by(order_by: Optional[str]) -> List[Tuple[str, bool]]:
  """Parses an AIP-132 order_by string into [(field, descending)]."""
  clauses = []
  for part in (order_by or "").split(","):
    tokens = part.split()
    if tokens:
      clauses.append((tokens[0], len(tokens) > 1 and
                      tokens[1].lower() == "desc"))
  return clauses


def sort_records(records: List[Any], get_value: Callable[[Any, str], Any],
                 clauses: List[Tuple[str, bool]]) -> List[Any]:
  """Sorts numbers before strings before nulls, per clause, stably."""

  def key(value: Any) -> Tuple[int, float, str]:
    if is_null(value):
      return (2, 0.0, "")
    try:
      return (0, float(value), "")
    except (TypeError, ValueError):
      return (1, 0.0, str(value).lower())

  result = list(records)
  for field, descending in reversed(clauses):
    result.sort(key=lambda r: key(get_value(r, field)), reverse=descending)
  return result


def parse_experiment_filter(raw: Any) -> Optional[Dict[str, Any]]:
  """Normalizes an experiment filter; None unless both ids are present."""
  if not isinstance(raw, dict):
    return None
  experiment_id = as_int(get_param(raw, "experiment_id"), None)
  control_id = as_int(get_param(raw, "control_id"), None)
  if experiment_id is None or control_id is None:
    return None
  return {
      "experimentId": experiment_id,
      "controlId": control_id,
      "isTreatment": as_bool(get_param(raw, "is_treatment", True)),
  }
