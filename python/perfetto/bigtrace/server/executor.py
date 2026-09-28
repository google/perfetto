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
"""Runs a query over many traces in parallel and tracks its results."""

import collections
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
import logging
import os
import threading
from typing import Any, Dict, List, Optional, Tuple
import uuid

from perfetto.trace_processor.api import (
    TraceProcessorConfig,
    TraceProcessorException,
)
from perfetto.bigtrace.server import query_result
from perfetto.bigtrace.server.catalog import (
    SettingsOverrides,
    TraceCatalog,
    TraceEntry,
)
from perfetto.bigtrace.server.filters import (
    evaluate_filters,
    parse_experiment_filter,
    parse_order_by,
    sort_records,
)
from perfetto.bigtrace.server.history import (
    BigtraceHistoryDb,
    STATUS_FIELDS,
    TERMINAL_STATUSES,
    public_view,
)
from perfetto.bigtrace.server.trace_processor_pool import (
    EphemeralTraceProcessorPool,
    KeepAliveTraceProcessorPool,
    TraceProcessorPool,
)
from perfetto.bigtrace.server.util import (
    as_int,
    get_param,
    infer_column_type,
    iso_now,
    to_wire,
)

logger = logging.getLogger(__name__)


class QueryFailedError(Exception):
  pass


class BigtraceQueryExecutor:
  """Runs PerfettoSQL over many traces in parallel and serves the results.

  An execution is a dict with the PUBLIC_FIELDS plus:
    _rows: result rows; SQL columns followed by trace metadata columns.
    _columns: names of the values in each row.
    _schema_map: column name to Bigtrace type.
    _cancel: threading.Event that stops the query.
    _active: TraceProcessors currently running the query.
    _claimed_rows: rows reserved by traces so far, up to `limit`.
  Running executions stay in memory. Finished ones are persisted, and the
  `cache_size` most recently used stay in memory for fast paging.
  """

  def __init__(
      self,
      catalog: TraceCatalog,
      tp_config: Optional[TraceProcessorConfig] = None,
      max_workers: int = 4,
      history_db: Optional[BigtraceHistoryDb] = None,
      keep_alive: bool = False,
      pool_size: int = 50,
      cache_size: int = 16,
  ):
    self.catalog = catalog
    self.history_db = history_db or BigtraceHistoryDb()
    self.cache_size = cache_size
    self.max_workers = max_workers
    tp_config = tp_config or TraceProcessorConfig()
    if keep_alive:
      self.pool: TraceProcessorPool = KeepAliveTraceProcessorPool(
          tp_config, pool_size)
    else:
      self.pool = EphemeralTraceProcessorPool(tp_config)
    self._workers = ThreadPoolExecutor(max_workers=max_workers)
    self._lock = threading.RLock()
    self._executions: "collections.OrderedDict[str, Dict[str, Any]]" = (
        collections.OrderedDict())
    self._runs: List[threading.Thread] = []

  def close(self) -> None:
    with self._lock:
      for ex in self._executions.values():
        self._stop(ex)
      runs = list(self._runs)
    self._workers.shutdown(wait=False, cancel_futures=True)
    # Let stopped queries record their final state before the database closes.
    for run in runs:
      run.join(timeout=10)
    self.pool.close()
    self.history_db.close()

  def _remember(self, ex: Dict[str, Any]) -> None:
    with self._lock:
      self._executions[ex["queryUuid"]] = ex
      self._executions.move_to_end(ex["queryUuid"])
      finished = [
          k for k, v in self._executions.items()
          if v["status"] in TERMINAL_STATUSES
      ]
      for k in finished[:max(0, len(finished) - self.cache_size)]:
        del self._executions[k]

  def _load(self, query_uuid: str) -> Dict[str, Any]:
    """Returns the execution with its rows, reading history if needed."""
    with self._lock:
      ex = self._executions.get(query_uuid)
      if ex:
        self._executions.move_to_end(query_uuid)
        return ex
    ex = self.history_db.get_execution(query_uuid)
    if ex is None:
      raise KeyError(query_uuid)
    ex["_rows"] = self.history_db.get_results(query_uuid)
    ex["_cancel"] = threading.Event()
    ex["_active"] = set()
    self._remember(ex)
    return ex

  def list_executions(self) -> List[Dict[str, Any]]:
    by_uuid = {ex["queryUuid"]: ex for ex in self.history_db.list_executions()}
    with self._lock:
      for query_uuid, ex in self._executions.items():
        by_uuid[query_uuid] = public_view(ex)
    return sorted(
        by_uuid.values(), key=lambda e: e.get("startTime") or "", reverse=True)

  def get_execution(self, query_uuid: str) -> Optional[Dict[str, Any]]:
    with self._lock:
      if query_uuid in self._executions:
        return public_view(self._executions[query_uuid])
    ex = self.history_db.get_execution(query_uuid)
    return public_view(ex) if ex else None

  def get_status(self, query_uuid: str) -> Optional[Dict[str, Any]]:
    with self._lock:
      if query_uuid in self._executions:
        return public_view(self._executions[query_uuid], STATUS_FIELDS)
    return self.history_db.get_status(query_uuid)

  def check_table_exists(self, table_name: str) -> Dict[str, Any]:
    exists = any(
        ex.get("tableName") == table_name for ex in self.list_executions())
    return {"exists": exists, "resolvedTableName": table_name}

  def _stop(self, ex: Dict[str, Any]) -> None:
    """Stops a running query, killing the trace processors running it."""
    with self._lock:
      cancel = ex.get("_cancel")
      if cancel:
        cancel.set()
      active = list(ex.get("_active", ()))
      if "_active" in ex:
        ex["_active"].clear()
    for tp in active:
      tp.close()

  def cancel_query(self, query_uuid: str) -> None:
    with self._lock:
      ex = self._executions.get(query_uuid)
      if ex:
        self._stop(ex)
        if ex["status"] == "IN_PROGRESS":
          ex["status"] = "CANCELLED"
          ex["endTime"] = iso_now()
    self.history_db.mark_cancelled(query_uuid)

  def delete_execution(self, query_uuid: str) -> bool:
    with self._lock:
      ex = self._executions.pop(query_uuid, None)
      if ex:
        self._stop(ex)
    return self.history_db.delete_execution(query_uuid) or ex is not None

  def execute(self,
              params: Dict[str, Any],
              is_async: bool = False) -> Dict[str, Any]:
    """Starts a query. Sync calls block and return the first result page."""
    sql = str(get_param(params, "perfetto_sql", ""))
    if not sql.strip():
      raise ValueError("perfetto_sql is required")
    limit = max(1, as_int(get_param(params, "limit"), 10000))
    trace_limit = as_int(get_param(params, "trace_limit"), 0) or None
    settings = get_param(params, "settings", [])
    trace_filters = get_param(params, "trace_filters", [])
    trace_order_by = get_param(params, "trace_order_by")
    experiment_filter = parse_experiment_filter(
        get_param(params, "experiment_filter"))

    overrides = SettingsOverrides.parse(settings)
    self.catalog.apply_overrides(overrides)
    traces, _ = self.catalog.filter_and_sort(
        filters=trace_filters,
        order_by=trace_order_by,
        experiment_filter=experiment_filter,
        trace_uuids=overrides.trace_uuids,
        limit=trace_limit,
    )

    query_uuid = str(uuid.uuid4())
    table_name = get_param(params, "table_name") or (
        f"bigtrace_{query_uuid.replace('-', '_')}" if is_async else None)
    ex: Dict[str, Any] = dict(
        queryUuid=query_uuid,
        status="IN_PROGRESS",
        startTime=iso_now(),
        endTime=None,
        processedRows=0,
        processedTraces=0,
        totalTraces=len(traces),
        perfettoSql=sql,
        limit=limit,
        traceLimit=trace_limit,
        materialized=bool(table_name),
        tableName=table_name,
        tableLink=None,
        schema=[],
        settings=[{
            "settingId": get_param(s, "setting_id", ""),
            "values": [str(v) for v in s.get("values") or []],
            "category": s.get("category", "TRACE_ADDRESS"),
        } for s in settings if isinstance(s, dict)],
        traceFilters=trace_filters,
        traceMetadataColumns=list(
            get_param(params, "trace_metadata_columns", [])),
        traceOrderBy=trace_order_by,
        experimentFilter=experiment_filter,
        error=None,
        errorMessage=None,
        _rows=[],
        _columns=[],
        _schema_map={},
        _cancel=threading.Event(),
        _active=set(),
        _claimed_rows=0,
    )
    self._remember(ex)
    self.history_db.save_execution(ex)

    if is_async:
      run = threading.Thread(target=self._run, args=(ex, traces), daemon=True)
      with self._lock:
        self._runs = [r for r in self._runs if r.is_alive()] + [run]
      run.start()
      return {
          "queryUuid": query_uuid,
          "schema": [],
          "rows": [],
          "totalFilteredRows": 0
      }
    self._run(ex, traces)
    if ex["status"] == "FAILED":
      raise QueryFailedError(ex["errorMessage"])
    return self.fetch_results(query_uuid, offset=0, limit=limit)

  def _wants_rows(self, ex: Dict[str, Any]) -> bool:
    with self._lock:
      return not ex["_cancel"].is_set() and ex["_claimed_rows"] < ex["limit"]

  def _claim_rows(self, ex: Dict[str, Any], count: int) -> int:
    """Reserves up to `count` of the query's remaining rows."""
    with self._lock:
      if ex["_cancel"].is_set():
        return 0
      granted = max(0, min(count, ex["limit"] - ex["_claimed_rows"]))
      ex["_claimed_rows"] += granted
      return granted

  def _query_trace(
      self, ex: Dict[str, Any],
      trace: TraceEntry) -> Optional[Tuple[List[str], List[List[Any]]]]:
    """Returns (columns, rows) for one trace, or None if skipped.

    Stops decoding once the query has `limit` rows across all traces. Raises
    if the file is not a trace or the query fails.
    """
    if (not self._wants_rows(ex) or trace.file_size == 0 or
        not os.path.isfile(trace.file_path)):
      return None
    try:
      tp = self.pool.acquire(trace.file_path, cancel_event=ex["_cancel"])
    except TraceProcessorException:
      if ex["_cancel"].is_set():
        return None
      raise
    if ex["_cancel"].is_set():
      self.pool.release(trace.file_path, tp, discard=True)
      return None
    with self._lock:
      ex["_active"].add(tp)
    healthy = False
    try:
      if not self._wants_rows(ex):
        healthy = True
        return None
      columns, batches = query_result.query(tp, ex["perfettoSql"])
      healthy = True
      rows: List[List[Any]] = []
      for decode in batches:
        if not self._wants_rows(ex):
          break
        batch = decode()
        rows.extend(batch[:self._claim_rows(ex, len(batch))])
      return columns, rows
    except TraceProcessorException:
      if ex["_cancel"].is_set():
        return None
      # The query was rejected; the instance itself is still usable.
      healthy = True
      raise
    finally:
      with self._lock:
        ex["_active"].discard(tp)
        # cancel_query() closes instances it interrupts; don't reuse those.
        killed = getattr(tp, "subprocess", None) is None
      self.pool.release(trace.file_path, tp, discard=killed or not healthy)

  def _run(self, ex: Dict[str, Any], traces: List[TraceEntry]) -> None:
    """Runs the query on every trace, appending rows to ex as they arrive."""
    limit, cancel = ex["limit"], ex["_cancel"]
    catalog_types = {c["name"]: c["type"] for c in self.catalog.get_schema()}
    rows: List[List[Any]] = ex["_rows"]
    sql_columns: Optional[List[str]] = None
    meta_columns: List[str] = []
    errors: List[str] = []
    succeeded = 0

    # Loaded traces first: they answer fast, and loading the others then evicts
    # traces this query already used rather than ones it still needs.
    ordered_traces = sorted(
        traces, key=lambda t: not self.pool.is_loaded(t.file_path))

    # Bounded in-flight concurrency prevents cold trace loads from thrashing
    # disk I/O, process tables, and memory when querying large sets of traces.
    max_active = min(self.max_workers, 16)
    active_futures: Dict[Any, TraceEntry] = {}
    trace_iter = iter(ordered_traces)

    def submit_next() -> bool:
      if cancel.is_set() or len(rows) >= limit:
        return False
      t = next(trace_iter, None)
      if t is None:
        return False
      active_futures[self._workers.submit(self._query_trace, ex, t)] = t
      return True

    for _ in range(max_active):
      if not submit_next():
        break

    while active_futures:
      done, _ = wait(active_futures.keys(), return_when=FIRST_COMPLETED)
      for future in done:
        trace = active_futures.pop(future)
        try:
          result = future.result()
        except Exception as e:  # pylint: disable=broad-except
          if not cancel.is_set():
            logger.info("Query failed on %s: %s", trace.file_name, e)
            errors.append(f"{trace.file_name}: {e}")
          result = None

        with self._lock:
          ex["processedTraces"] += 1
          if result is not None:
            columns, trace_rows = result
            if sql_columns is None:
              sql_columns = columns
              meta_columns = [c for c in catalog_types if c not in columns]
              ex["_columns"] = sql_columns + meta_columns
              ex["_schema_map"] = {c: catalog_types[c] for c in meta_columns}
            if columns == sql_columns:
              succeeded += 1
              meta = [trace.get_value(c) for c in meta_columns]
              rows.extend(r + meta for r in trace_rows[:limit - len(rows)])
              ex["processedRows"] = len(rows)
              self._update_schema(ex, sql_columns, trace_rows)
            else:
              errors.append(f"{trace.file_name}: returned different columns")

        if cancel.is_set() or len(rows) >= limit:
          cancel.set()
          for f in active_futures:
            f.cancel()
          break

        submit_next()

    with self._lock:
      if ex["status"] != "CANCELLED":
        if errors and succeeded == 0:
          ex["status"] = "FAILED"
          ex["errorMessage"] = "; ".join(errors[:3])
        else:
          ex["status"] = "SUCCESS"
      ex["endTime"] = iso_now()
    self.history_db.save_execution(ex)
    if ex["status"] != "CANCELLED":
      self.history_db.save_results(ex["queryUuid"], rows)
    self._remember(ex)

  @staticmethod
  def _update_schema(ex: Dict[str, Any], sql_columns: List[str],
                     trace_rows: List[List[Any]]) -> None:
    """Types each SQL column by the first non-null value any trace returns."""
    schema_map = ex["_schema_map"]
    for i, col in enumerate(sql_columns):
      if col not in schema_map:
        sample = next((r[i] for r in trace_rows if r[i] is not None), None)
        if sample is not None:
          schema_map[col] = infer_column_type([sample])
    visible = sql_columns + [
        c for c in ex["traceMetadataColumns"] if c not in sql_columns
    ]
    ex["schema"] = [{
        "name": c,
        "type": schema_map.get(c, "STRING")
    } for c in visible]

  def fetch_results(
      self,
      query_uuid: str,
      offset: int = 0,
      limit: int = 50,
      order_by: Optional[str] = None,
      filters: Optional[List[Dict[str, Any]]] = None,
      columns: Optional[List[str]] = None,
  ) -> Dict[str, Any]:
    """Returns a filtered, sorted and projected page of results."""
    ex = self._load(query_uuid)
    with self._lock:
      rows = list(ex["_rows"])
      all_columns = list(ex["_columns"])
      schema_map = dict(ex["_schema_map"])
      visible = [s["name"] for s in ex["schema"]]
    index = {c: i for i, c in enumerate(all_columns)}

    def get(row: List[Any], col: str) -> Any:
      return row[index[col]] if col in index else None

    if filters:
      rows = [r for r in rows if evaluate_filters(lambda c: get(r, c), filters)]
    total = len(rows)
    if order_by:
      rows = sort_records(rows, get, parse_order_by(order_by))
    offset, limit = max(0, offset), max(0, limit)
    out_columns = [c for c in (columns or visible or all_columns) if c in index]
    return {
        "queryUuid": query_uuid,
        "schema": [{
            "name": c,
            "type": schema_map.get(c, "STRING")
        } for c in out_columns],
        "rows": [{
            "values": [to_wire(r[index[c]]) for c in out_columns]
        } for r in rows[offset:offset + limit]],
        "totalFilteredRows": total,
        "availableColumnNames": all_columns,
    }
