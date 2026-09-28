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
import json
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
import logging
import os
import sqlite3
import threading
from typing import Any, Callable, Dict, List, Optional, Tuple
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
from perfetto.bigtrace.server.results import ResultSet
from perfetto.bigtrace.server.snapshot import SnapshotStore
from perfetto.bigtrace.server.trace_processor_pool import (
    PoolQuery,
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
    _rows: ResultSet of rows; SQL columns followed by trace metadata columns.
    _columns: names of the values in each row.
    _schema_map: column name to Bigtrace type.
    _cancel: threading.Event that stops the query.
    _active: TraceProcessors currently running the query.
    _claimed_rows: rows reserved by traces so far, up to `limit`.
    _deleted: set once deleted, so a running query doesn't save it again.
  Running executions stay in memory. Finished ones are persisted, and the
  `cache_size` most recently used stay in memory for fast paging.
  """

  def __init__(
      self,
      catalog: TraceCatalog,
      tp_config: Optional[TraceProcessorConfig] = None,
      max_workers: int = 4,
      history_db: Optional[BigtraceHistoryDb] = None,
      pool_size: int = 0,
      cache_size: int = 16,
      snapshots: Optional[SnapshotStore] = None,
  ):
    self.catalog = catalog
    self.history_db = history_db or BigtraceHistoryDb()
    self.cache_size = cache_size
    self.max_workers = max_workers
    self.pool = TraceProcessorPool(tp_config, pool_size, snapshots)
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
    # Kills every shell, so running queries and loads end promptly.
    self.pool.close()
    self._workers.shutdown(wait=True, cancel_futures=True)
    # Let stopped queries record their final state before the database closes.
    for run in runs:
      run.join(timeout=10)
    self.history_db.close()

  def _persist(self, write: Callable[..., None], *args: Any) -> None:
    """Runs a history write. Failing (e.g. disk full) only loses history."""
    try:
      write(*args)
    except (sqlite3.Error, OSError) as e:
      logger.warning("Could not save query history: %s", e)

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
    ex["_rows"] = ResultSet.from_records(
        self.history_db.get_results(query_uuid))
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
    self._persist(self.history_db.mark_cancelled, query_uuid)

  def delete_execution(self, query_uuid: str) -> bool:
    with self._lock:
      ex = self._executions.pop(query_uuid, None)
      if ex:
        ex["_deleted"] = True
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
        _rows=ResultSet(),
        _columns=[],
        _schema_map={},
        _cancel=threading.Event(),
        _active=set(),
        _claimed_rows=0,
    )
    self._remember(ex)
    self._persist(self.history_db.save_execution, ex)

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

  @staticmethod
  def _wants_rows(ex: Dict[str, Any]) -> bool:
    # A racy read is fine: _claim_rows enforces the limit under the lock.
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
      self, ex: Dict[str, Any], trace: TraceEntry, pool_query: PoolQuery
  ) -> Optional[Tuple[List[str], List[Tuple[bytes, int]]]]:
    """Returns (columns, [(batch, row count)]) for one trace, or None.

    Stops decoding once the query has `limit` rows across all traces. Raises
    if the file is not a trace or the query fails.
    """
    if not self._wants_rows(ex):
      return None
    if not os.path.isfile(trace.file_path):
      raise FileNotFoundError("trace file not found")
    try:
      tp = self.pool.acquire(trace.file_path, ex["_cancel"], pool_query)
    except TraceProcessorException:
      if ex["_cancel"].is_set():
        return None
      raise
    if not self._wants_rows(ex):
      # The pool counts the query as run on this instance; it was not.
      self.pool.release(trace.file_path, tp, discard=True)
      return None
    with self._lock:
      ex["_active"].add(tp)
    healthy = False
    try:
      result = query_result.query(tp, ex["perfettoSql"])
      healthy = True
      if result.is_stateful:
        self.pool.add_to_session(pool_query)
      # Keep batches encoded (see results.py); only count their rows.
      kept: List[Tuple[bytes, int]] = []
      for batch in result.batches:
        if not self._wants_rows(ex):
          break
        granted = self._claim_rows(
            ex, query_result.row_count(batch, len(result.columns)))
        if granted:
          kept.append((bytes(batch), granted))
      return result.columns, kept
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
    results: ResultSet = ex["_rows"]
    sql_columns: Optional[List[str]] = None
    meta_columns: List[str] = []
    errors: List[str] = []
    succeeded = 0
    pool_query = self.pool.start_query(ex["perfettoSql"])
    try:
      # Loaded traces first: they answer fast, and loading the others then evicts
      # traces this query already used rather than ones it still needs.
      ordered_traces = sorted(
          traces, key=lambda t: not self.pool.is_loaded(t.file_path))

      # Submit no more than the workers can run, so cancelling or reaching the
      # limit stops promptly. max_workers is bounded by cores and memory.
      max_active = self.max_workers
      active_futures: Dict[Any, TraceEntry] = {}
      trace_iter = iter(ordered_traces)

      def submit_next() -> bool:
        if cancel.is_set() or len(results) >= limit:
          return False
        t = next(trace_iter, None)
        if t is None:
          return False
        active_futures[self._workers.submit(self._query_trace, ex, t,
                                            pool_query)] = t
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

          if result is not None:
            columns, batches = result
            if sql_columns is None:
              sql_columns = columns
              meta_columns = [c for c in catalog_types if c not in columns]
              with self._lock:
                ex["_columns"] = sql_columns + meta_columns
                ex["_schema_map"] = {c: catalog_types[c] for c in meta_columns}
            if columns == sql_columns:
              succeeded += 1
              # Rows were claimed against the limit, so they all fit.
              results.add(batches, len(columns),
                          [trace.get_value(c) for c in meta_columns])
              self._update_schema(ex, sql_columns, batches)
            else:
              errors.append(f"{trace.file_name}: returned different columns")

          with self._lock:
            ex["processedTraces"] += 1
            ex["processedRows"] = len(results)

          if cancel.is_set() or len(results) >= limit:
            cancel.set()
            for f in active_futures:
              f.cancel()
            break

          submit_next()

    finally:
      self.pool.end_query()
    with self._lock:
      if ex["status"] != "CANCELLED":
        # Some traces failing does not fail the query; it is still reported.
        if errors:
          ex["errorMessage"] = (f"{len(errors)} trace(s) failed: " +
                                "; ".join(errors[:3]))
        ex["status"] = "FAILED" if errors and succeeded == 0 else "SUCCESS"
      ex["endTime"] = iso_now()
      if ex.get("_deleted"):
        return
    self._persist(self.history_db.save_execution, ex)
    if ex["status"] != "CANCELLED":
      self._persist(self.history_db.save_results, ex["queryUuid"],
                    results.to_records())
    self._remember(ex)

  def _update_schema(self, ex: Dict[str, Any], sql_columns: List[str],
                     batches: List[Tuple[bytes, int]]) -> None:
    """Types each SQL column by the first non-null value any trace returns."""
    # Only the query's own thread writes the schema; others read it locked.
    schema_map = ex["_schema_map"]
    untyped = [i for i, c in enumerate(sql_columns) if c not in schema_map]
    types: Dict[str, str] = {}
    if untyped and batches:
      # A trace's first batch is enough to type its columns.
      sample = query_result.decode_batch(batches[0][0], len(sql_columns))
      for i in untyped:
        value = next((r[i] for r in sample if r[i] is not None), None)
        if value is not None:
          types[sql_columns[i]] = infer_column_type([value])
    with self._lock:
      schema_map.update(types)
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
    results: ResultSet = ex["_rows"]
    with self._lock:
      all_columns = list(ex["_columns"])
      schema_map = dict(ex["_schema_map"])
      visible = [s["name"] for s in ex["schema"]]
    index = {c: i for i, c in enumerate(all_columns)}

    def get(row: List[Any], col: str) -> Any:
      return row[index[col]] if col in index else None

    offset, limit = max(0, offset), max(0, limit)
    if filters or order_by:
      # Paging through a sorted view must not re-sort every row per page.
      key = (json.dumps(filters, sort_keys=True), order_by, len(results))
      view = ex.get("_view")
      if not view or view[0] != key:
        rows = results.rows()
        if filters:
          rows = [
              r for r in rows if evaluate_filters(lambda c: get(r, c), filters)
          ]
        if order_by:
          rows = sort_records(rows, get, parse_order_by(order_by))
        view = ex["_view"] = (key, rows)
      rows = view[1]
      total, page = len(rows), rows[offset:offset + limit]
    else:
      total, page = len(results), results.page(offset, limit)
    out_columns = [c for c in (columns or visible or all_columns) if c in index]
    return {
        "queryUuid": query_uuid,
        "schema": [{
            "name": c,
            "type": schema_map.get(c, "STRING")
        } for c in out_columns],
        "rows": [{
            "values": [to_wire(r[index[c]]) for c in out_columns]
        } for r in page],
        "totalFilteredRows": total,
        "availableColumnNames": all_columns,
    }
