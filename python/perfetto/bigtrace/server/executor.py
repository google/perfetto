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
"""Runs queries over traces or DuckDB tables, materializing results in DuckDB."""

import collections
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
import logging
import os
import re
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Set, Tuple
import uuid

from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.bigtrace.server.catalog import (
    SettingsOverrides,
    TraceCatalog,
    TraceEntry,
)
from perfetto.bigtrace.server.duckdb_engine import (
    DuckDBEngine,
    STATUS_FIELDS,
    TERMINAL_STATUSES,
    public_view,
)
from perfetto.bigtrace.server.filters import parse_experiment_filter
from perfetto.bigtrace.server import stream_ingest
from perfetto.bigtrace.server.stream_ingest import FeedResult, StreamIngest
from perfetto.bigtrace.server.trace_processor_pool import (
    Shell,
    ShellCancelled,
    ShellStartError,
    TraceProcessorPool,
)
from perfetto.bigtrace.server.util import (
    as_int,
    get_param,
    iso_now,
    to_wire,
)

logger = logging.getLogger(__name__)

_DUCKDB_DIRECTIVE = re.compile(r"^\s*--\s*(?:@duckdb|engine\s*:\s*duckdb)\b",
                               re.IGNORECASE)
# Holds trace ords in result tables whose query has its own `_trace_uuid`
# column, so the rows of failed traces can be deleted; dropped at the end.
_ORD_COLUMN = "_bt_trace_ord"


class QueryFailedError(Exception):
    pass


class BigtraceQueryExecutor:
    """Runs PerfettoSQL over traces or DuckDB SQL over materialized tables.

  Every query streams its rows directly into DuckDB (`result_1`, `result_2`,
  or a custom `table_name`, plus `latest`) and serves UI pages directly from
  DuckDB.
  """

    def __init__(
        self,
        catalog: TraceCatalog,
        tp_config: Optional[TraceProcessorConfig] = None,
        max_workers: int = 4,
        cache_size: int = 16,
        duckdb_engine: Optional[DuckDBEngine] = None,
    ):
        self.catalog = catalog
        self.cache_size = cache_size
        self.max_workers = max_workers
        self.pool = TraceProcessorPool(tp_config)
        self.duckdb = duckdb_engine or DuckDBEngine()
        self.duckdb.sync_catalog(self.catalog)
        self._workers = ThreadPoolExecutor(max_workers=max_workers)
        self._lock = threading.RLock()
        self._executions: "collections.OrderedDict[str, Dict[str, Any]]" = (
            collections.OrderedDict())
        self._runs: List[threading.Thread] = []

    def close(self) -> None:
        with self._lock:
            exs = list(self._executions.values())
            runs = list(self._runs)
        for ex in exs:
            self._stop(ex)
        self.pool.close()
        self._workers.shutdown(wait=True, cancel_futures=True)
        for run in runs:
            run.join(timeout=10)
        self.duckdb.close()

    def _persist(self, write: Callable[..., Any], *args: Any) -> None:
        try:
            write(*args)
        except Exception as e:  # pylint: disable=broad-except
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
        with self._lock:
            ex = self._executions.get(query_uuid)
            if ex:
                self._executions.move_to_end(query_uuid)
                return ex
        ex = self.duckdb.get_execution(query_uuid)
        if ex is None:
            raise KeyError(query_uuid)
        ex["_cancel"] = threading.Event()
        ex["_active"] = set()
        self._remember(ex)
        return ex

    def list_executions(self) -> List[Dict[str, Any]]:
        by_uuid = {ex["queryUuid"]: ex for ex in self.duckdb.list_executions()}
        with self._lock:
            for query_uuid, ex in self._executions.items():
                by_uuid[query_uuid] = public_view(ex)
        return sorted(by_uuid.values(),
                      key=lambda e: e.get("startTime") or "",
                      reverse=True)

    def get_execution(self, query_uuid: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            if query_uuid in self._executions:
                return public_view(self._executions[query_uuid])
        ex = self.duckdb.get_execution(query_uuid)
        return public_view(ex) if ex else None

    def get_status(self, query_uuid: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            if query_uuid in self._executions:
                ex = self._executions[query_uuid]
                if (ex.get("status") == "IN_PROGRESS" and
                        ex.get("queryEngine") == "duckdb"):
                    pct = -1.0
                    cur = ex.get("_duck_cursor")
                    if cur is not None and hasattr(cur, "query_progress"):
                        try:
                            pct = max(pct, float(cur.query_progress()))
                        except Exception:  # pylint: disable=broad-except
                            pass
                    if self.duckdb.stream_api is not None:
                        try:
                            pct = max(
                                pct,
                                float(
                                    self.duckdb.stream_api.get_query_progress()
                                ),
                            )
                        except Exception:  # pylint: disable=broad-except
                            pass
                    if pct >= 0.0:
                        ex["processedTraces"] = max(
                            int(ex.get("processedTraces") or 0),
                            min(99, int(round(pct))),
                        )
                        ex["totalTraces"] = 100
                return public_view(ex, STATUS_FIELDS)
        return self.duckdb.get_status(query_uuid)

    def check_table_exists(self, table_name: str) -> Dict[str, Any]:
        exists_duck, resolved = self.duckdb.table_exists(table_name)
        all_exs = self.list_executions()
        in_flight = any(
            ex.get("status") == "IN_PROGRESS" and ex.get("tableName") in (
                table_name, resolved) for ex in all_exs)
        hist_names = {
            ex["tableName"]
            for ex in all_exs
            if ex.get("tableName") and (ex.get("status") == "IN_PROGRESS" or
                                        (ex.get("processedRows") or 0) > 0)
        }
        base = resolved or table_name
        suggested = self.duckdb.suggest_table_name(base, reserved=hist_names)
        return {
            "exists": exists_duck or in_flight,
            "resolvedTableName": base,
            "suggestedTableName": suggested,
        }

    def _stop(self, ex: Dict[str, Any]) -> None:
        """Stops a running query, killing the trace processors running it."""
        with self._lock:
            cancel = ex.get("_cancel")
            if cancel:
                cancel.set()
            batch_cancel = ex.get("_batch_cancel")
            if isinstance(batch_cancel, threading.Event):
                if all(
                        e.get("_cancel", threading.Event()).is_set()
                        for e in self._executions.values()
                        if e.get("_batch_cancel") is batch_cancel):
                    batch_cancel.set()
            active = list(ex.get("_active", ()))
            if "_active" in ex:
                ex["_active"].clear()
            shared = {
                shell for other in self._executions.values()
                if other is not ex and not other["_cancel"].is_set()
                for shell in other.get("_active", ())
            }
            to_close = [shell for shell in active if shell not in shared]
            duck_cursor = ex.get("_duck_cursor")
            ingest: Optional[StreamIngest] = ex.get("_ingest")
        if ingest is not None:
            ingest.cancel()
        for shell in to_close:
            shell.close()
        if duck_cursor is not None:
            threading.Thread(target=self._interrupt_until_done,
                             args=(ex,),
                             daemon=True).start()

    def _interrupt_until_done(self, ex: Dict[str, Any]) -> None:
        """Interrupts `ex`'s DuckDB cursor until its run ends.

    A single interrupt() is lost if it lands before the cursor's next
    statement starts, so keep interrupting while the run holds a cursor.
    """
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            with self._lock:
                cur = ex.get("_duck_cursor")
            if cur is None:
                return
            try:
                cur.interrupt()
            except Exception:  # pylint: disable=broad-except
                pass
            time.sleep(0.05)

    def cancel_query(self, query_uuid: str) -> None:
        with self._lock:
            ex = self._executions.get(query_uuid)
            if ex and ex["status"] == "IN_PROGRESS":
                ex["status"] = "CANCELLED"
                ex["endTime"] = iso_now()
        if ex:
            self._stop(ex)
        self._persist(self.duckdb.mark_cancelled, query_uuid)

    def delete_execution(self, query_uuid: str) -> bool:
        table_name = None
        with self._lock:
            ex = self._executions.pop(query_uuid, None)
            if ex:
                table_name = ex.get("tableName")
                ex["_deleted"] = True
        if ex:
            self._stop(ex)
        if not table_name:
            stored = self.duckdb.get_execution(query_uuid)
            if stored:
                table_name = stored.get("tableName")
        self.duckdb.drop_table(table_name, query_uuid)
        return self.duckdb.delete_execution_record(query_uuid) or ex is not None

    @staticmethod
    def _parse_query_specs(
        params: Dict[str, Any],
        default_sql: str,
        default_table: Optional[str],
        default_limit: int,
    ) -> List[Tuple[str, Optional[str], int]]:
        raw_queries = get_param(params, "queries")
        specs: List[Tuple[str, Optional[str], int]] = []
        if isinstance(raw_queries, list) and raw_queries:
            for item in raw_queries:
                if isinstance(item, dict):
                    q_sql = str(
                        get_param(item, "perfetto_sql", "") or
                        get_param(item, "sql", ""))
                    q_tbl = get_param(item, "table_name")
                    q_lim = max(0,
                                as_int(get_param(item, "limit"), default_limit))
                    if q_sql.strip():
                        specs.append(
                            (q_sql, str(q_tbl) if q_tbl else None, q_lim))
                elif isinstance(item, str) and item.strip():
                    specs.append((item, None, default_limit))
        if not specs and default_sql.strip():
            specs.append(
                (default_sql, str(default_table) if default_table else None,
                 default_limit))
        return specs

    def execute(self,
                params: Dict[str, Any],
                is_async: bool = False) -> Dict[str, Any]:
        """Starts one or more queries. Sync calls block and return the first page."""
        sql = str(get_param(params, "perfetto_sql", ""))
        req_table = get_param(params, "table_name")
        limit = max(0, as_int(get_param(params, "limit"), 10000))
        specs = self._parse_query_specs(params, sql,
                                        str(req_table) if req_table else None,
                                        limit)
        if not specs:
            raise ValueError("perfetto_sql is required")

        first_sql = specs[0][0]
        engine = str(
            get_param(params, "engine", "") or
            get_param(params, "query_engine", "")).lower().strip()
        if not engine and _DUCKDB_DIRECTIVE.match(first_sql):
            engine = "duckdb"
        engine = "duckdb" if engine == "duckdb" else "traces"

        trace_limit = as_int(get_param(params, "trace_limit"), 0) or None
        settings = get_param(params, "settings", [])
        trace_filters = get_param(params, "trace_filters", [])
        trace_order_by = get_param(params, "trace_order_by")
        experiment_filter = parse_experiment_filter(
            get_param(params, "experiment_filter"))

        traces: List[TraceEntry] = []
        if engine == "traces":
            overrides = SettingsOverrides.parse(settings)
            if self.catalog.apply_overrides(overrides):
                self.duckdb.sync_catalog(self.catalog)
            traces, _ = self.catalog.filter_and_sort(
                filters=trace_filters,
                order_by=trace_order_by,
                experiment_filter=experiment_filter,
                trace_uuids=overrides.trace_uuids,
                limit=trace_limit,
            )

        req_meta = list(get_param(params, "trace_metadata_columns", []))
        if engine == "traces" and "_trace_uuid" not in req_meta:
            req_meta = req_meta + ["_trace_uuid"]

        norm_settings = [{
            "settingId": get_param(s, "setting_id", ""),
            "values": [str(v) for v in s.get("values") or []],
            "category": s.get("category", "TRACE_ADDRESS"),
        } for s in settings if isinstance(s, dict)]

        batch_cancel = threading.Event()
        executions: List[Dict[str, Any]] = []
        with self._lock:
            reserved_tables = {
                e["tableName"]
                for e in self._executions.values()
                if e.get("status") == "IN_PROGRESS" and e.get("tableName")
            }
            for q_sql, q_table, q_lim in specs:
                q_uuid = str(uuid.uuid4())
                t_name = self.duckdb.next_table_name(q_table,
                                                     reserved=reserved_tables)
                reserved_tables.add(t_name)
                ex: Dict[str, Any] = dict(
                    queryUuid=q_uuid,
                    status="IN_PROGRESS",
                    startTime=iso_now(),
                    endTime=None,
                    processedRows=0,
                    processedTraces=0,
                    totalTraces=len(traces) if engine == "traces" else 1,
                    perfettoSql=q_sql,
                    queryEngine=engine,
                    limit=q_lim,
                    traceLimit=trace_limit,
                    materialized=True,
                    tableName=t_name,
                    tableLink=None,
                    schema=[],
                    settings=norm_settings,
                    traceFilters=trace_filters,
                    traceMetadataColumns=req_meta,
                    traceOrderBy=trace_order_by,
                    experimentFilter=experiment_filter,
                    error=None,
                    errorMessage=None,
                    _columns=[],
                    _schema_map={},
                    _cancel=threading.Event(),
                    _batch_cancel=batch_cancel,
                    _active=set(),
                )
                self._remember(ex)
                executions.append(ex)
        for ex in executions:
            self._persist(self.duckdb.save_execution, ex)

        primary = executions[0]
        if engine == "duckdb":
            target = self._run_duckdb_batch
            args: Tuple[Any, ...] = (executions,)
        else:
            target = self._run_batch
            args = (executions, traces)

        if is_async:
            run = threading.Thread(target=target, args=args, daemon=True)
            with self._lock:
                self._runs = [r for r in self._runs if r.is_alive()] + [run]
            run.start()
            return {
                "queryUuid": primary["queryUuid"],
                "tableName": primary["tableName"],
                "executions": [{
                    "queryUuid": e["queryUuid"],
                    "tableName": e["tableName"],
                } for e in executions],
                "schema": [],
                "rows": [],
                "totalFilteredRows": 0,
            }
        target(*args)
        successful = next((e for e in executions if e["status"] == "SUCCESS"),
                          None)
        if not successful and primary["status"] == "FAILED":
            raise QueryFailedError(primary["errorMessage"])
        active_primary = successful or primary
        sync_limit = (active_primary["limit"] if active_primary["limit"] > 0
                      else active_primary["processedRows"])
        res = self.fetch_results(active_primary["queryUuid"],
                                 offset=0,
                                 limit=sync_limit)
        res["executions"] = [{
            "queryUuid": e["queryUuid"],
            "tableName": e["tableName"],
        } for e in executions]
        return res

    def _run_duckdb_batch(self, executions: List[Dict[str, Any]]) -> None:
        for ex in executions:
            self._run_duckdb(ex)

    def _run_duckdb(self, ex: Dict[str, Any]) -> None:
        """Runs a DuckDB query, writing its results into a DuckDB table."""

        def on_cursor(cur: Any) -> None:
            with self._lock:
                if ex["_cancel"].is_set():
                    raise QueryFailedError("cancelled")
                ex["_duck_cursor"] = cur

        try:
            columns, schema_map, row_count = self.duckdb.execute_into_table(
                ex["perfettoSql"],
                ex["tableName"],
                ex["limit"],
                query_uuid=ex["queryUuid"],
                on_cursor=on_cursor,
            )
            with self._lock:
                ex.pop("_duck_cursor", None)
                ex["_columns"] = columns
                ex["_schema_map"] = schema_map
                ex["schema"] = [{
                    "name": c,
                    "type": schema_map.get(c, "STRING")
                } for c in columns]
                ex["processedTraces"] = 1
                ex["processedRows"] = row_count
                if ex["status"] != "CANCELLED":
                    ex["status"] = "SUCCESS"
        except Exception as e:  # pylint: disable=broad-except
            has_table, _ = self.duckdb.table_exists(ex.get("tableName") or "")
            with self._lock:
                ex.pop("_duck_cursor", None)
                if not has_table:
                    ex["tableName"] = None
                if ex["status"] != "CANCELLED":
                    ex["status"] = "FAILED"
                    ex["errorMessage"] = f"DuckDB error: {e}"
        with self._lock:
            ex["endTime"] = iso_now()
            deleted = ex.get("_deleted")
        if deleted:
            self.duckdb.drop_table(ex.get("tableName"), ex["queryUuid"])
            return
        self._persist(self.duckdb.save_execution, ex)
        self._remember(ex)

    # Trace queries -------------------------------------------------------------

    def _new_run(self, ex: Dict[str, Any], traces: List[TraceEntry],
                 catalog_types: Dict[str, str]) -> "_TraceRun":
        assert self.duckdb.stream_api is not None
        run = _TraceRun(ex, traces)
        uuids = [t.trace_uuid for t in traces]
        scan_threads = stream_ingest.default_scan_threads()

        def on_schema(columns: List[str],
                      types: List[str]) -> Callable[[int], int]:
            with_uuid = "_trace_uuid" not in columns
            uuid_enum = with_uuid and self.duckdb.uuid_enum_covers(uuids)
            table, out_names = self.duckdb.create_result_table(
                ex["tableName"],
                columns,
                types,
                with_uuid=with_uuid,
                uuid_enum=uuid_enum,
                ord_column=None if with_uuid else _ORD_COLUMN,
                query_uuid=ex["queryUuid"],
                sql=ex["perfettoSql"])
            meta = ["_trace_uuid"
                   ] + [c for c in catalog_types if c != "_trace_uuid"]
            meta = [c for c in meta if c not in columns]
            with self._lock:
                run.table = table
                run.columns = list(columns)
                run.with_uuid = with_uuid
                ex["tableName"] = table
                ex["_columns"] = list(columns) + meta
                ex["_schema_map"] = {c: catalog_types[c] for c in meta}
                ex["_schema_map"].update(zip(columns, types))
            self._update_schema(ex, columns)

            def insert(handle: int) -> int:
                return self.duckdb.insert_from_stream(
                    table,
                    handle,
                    out_names,
                    types,
                    uuids=uuids if with_uuid else None,
                    uuid_enum=uuid_enum,
                    ord_column=None if with_uuid else _ORD_COLUMN,
                    scan_threads=scan_threads)

            return insert

        run.ingest = StreamIngest(self.duckdb.stream_api,
                                  ex["perfettoSql"],
                                  on_schema,
                                  row_limit=ex["limit"])
        with self._lock:
            ex["_ingest"] = run.ingest
        return run

    def _query_trace(self, runs: List["_TraceRun"], trace: TraceEntry,
                     trace_ord: int,
                     batch_cancel: threading.Event) -> List["_Outcome"]:
        """Loads `trace` once and feeds each run's query on it."""
        outcomes: List[_Outcome] = [(None, None)] * len(runs)
        if not any(r.wants_more() for r in runs):
            return outcomes
        if not os.path.isfile(trace.file_path):
            return [(None, FileNotFoundError("trace file not found"))
                   ] * len(runs)
        try:
            shell = self.pool.acquire(trace.file_path, batch_cancel)
        except ShellCancelled:
            return outcomes
        except ShellStartError as e:
            return [(None, e)] * len(runs)
        with self._lock:
            for r in runs:
                r.ex["_active"].add(shell)
        try:
            for i, run in enumerate(runs):
                if not run.wants_more():
                    continue
                if shell.closed:
                    break  # Killed by a cancel.
                res = run.ingest.feed(shell.host, shell.port, trace_ord)
                outcomes[i] = (res, None)
                if res.status == stream_ingest.FEED_IO_ERROR:
                    if shell.proc.poll() is not None:
                        err = RuntimeError(
                            f"trace processor died: {res.message}")
                        for j in range(i + 1, len(runs)):
                            outcomes[j] = (None, err)
                        break
            return outcomes
        finally:
            with self._lock:
                for r in runs:
                    r.ex["_active"].discard(shell)
            self.pool.release(shell)

    def _run_batch(self, executions: List[Dict[str, Any]],
                   traces: List[TraceEntry]) -> None:
        """Runs 1..N queries per trace in a single pass, streaming into DuckDB."""
        runs: List[_TraceRun] = []
        futures: Dict[Future, Tuple[int, TraceEntry]] = {}
        try:
            if self.duckdb.stream_api is None:
                raise QueryFailedError(
                    "The perfetto DuckDB extension is not loaded")
            self.duckdb.sync_catalog(self.catalog)
            catalog_types = {
                c["name"]: c["type"] for c in self.catalog.get_schema()
            }
            catalog_types["_trace_uuid"] = "STRING"
            for ex in executions:
                runs.append(self._new_run(ex, traces, catalog_types))
            with self._lock:
                batch_cancel = executions[0].get(
                    "_batch_cancel") or threading.Event()
                for ex in executions:
                    ex["_batch_cancel"] = batch_cancel
            pending = iter(enumerate(traces))

            def submit_next() -> bool:
                if not any(r.wants_more() for r in runs):
                    return False
                item = next(pending, None)
                if item is None:
                    return False
                trace_ord, trace = item
                futures[self._workers.submit(self._query_trace, runs, trace,
                                             trace_ord, batch_cancel)] = item
                return True

            for _ in range(self.max_workers):
                if not submit_next():
                    break
            while futures:
                done, _ = wait(list(futures), return_when=FIRST_COMPLETED)
                for future in done:
                    trace_ord, trace = futures.pop(future)
                    try:
                        outcomes = future.result()
                    except Exception as e:  # pylint: disable=broad-except
                        outcomes = [(None, e)] * len(runs)
                    for run, outcome in zip(runs, outcomes):
                        run.record(self._lock, trace_ord, trace, outcome)
                    submit_next()
                if not any(r.wants_more() for r in runs):
                    batch_cancel.set()  # Stops loads in flight.
        except BaseException as e:  # pylint: disable=broad-except
            logger.exception("Trace query failed")
            for run in runs:
                run.fatal = run.fatal or f"Internal error: {e}"
                run.ingest.cancel()
            for ex in executions:
                if not runs:
                    ex["errorMessage"] = str(e)
        finally:
            # Late feeds must not write after the tables are finalized.
            for future in futures:
                future.cancel()
            wait(list(futures))
            for i, ex in enumerate(executions):
                run = runs[i] if i < len(runs) else None
                try:
                    self._finish_run(ex, run)
                except Exception as e:  # pylint: disable=broad-except
                    logger.exception("Finalizing query %s failed",
                                     ex["queryUuid"])
                    with self._lock:
                        if ex["status"] == "IN_PROGRESS":
                            ex["status"] = "FAILED"
                            ex["errorMessage"] = f"Internal error: {e}"
                        ex["endTime"] = iso_now()
                    self._persist(self.duckdb.save_execution, ex)

    def _finish_run(self, ex: Dict[str, Any],
                    run: Optional["_TraceRun"]) -> None:
        """Waits for `run`'s inserts, fixes up its table and sets its status."""
        errors: List[str] = []
        table = None
        if run is not None:
            result = run.ingest.finish()
            errors = list(run.errors)
            for trace_ord in result.failed_ords:
                run.succeeded.discard(trace_ord)
            errors += result.errors
            if run.fatal:
                errors.append(run.fatal)
            table = run.table
        with self._lock:
            cancelled = ex["status"] == "CANCELLED"
            deleted = bool(ex.get("_deleted"))
            succeeded = bool(run and run.succeeded)
            if not cancelled:
                status = "FAILED" if errors and not succeeded else "SUCCESS"
            else:
                status = "CANCELLED"
            if errors and not cancelled:
                ex["errorMessage"] = (f"{len(errors)} trace(s) failed: " +
                                      "; ".join(errors[:3]))
            elif not ex.get("errorMessage") and not run:
                ex["errorMessage"] = "Query failed"
        materialize = table is not None and not deleted and (
            status == "SUCCESS" or (status == "CANCELLED" and succeeded))
        rows = 0
        if table is not None and materialize:
            assert run is not None
            if run.partial:
                if run.with_uuid:
                    self.duckdb.delete_trace_rows(
                        table,
                        uuids=[run.traces[o].trace_uuid for o in run.partial])
                else:
                    self.duckdb.delete_trace_rows(table,
                                                  ords=sorted(run.partial),
                                                  ord_column=_ORD_COLUMN)
            if not run.with_uuid:
                self.duckdb.drop_column(table, _ORD_COLUMN)
            retype = {}
            for col, mask in zip(run.columns, result.cell_types):
                btype = stream_ingest.type_from_cell_types(mask)
                if col in run.ingest.untyped and btype and btype != "STRING":
                    retype[col] = btype
            if retype:
                self.duckdb.retype_columns(table, retype)
                with self._lock:
                    ex["_schema_map"].update(retype)
                self._update_schema(ex, run.columns)
            self.duckdb.finalize_streaming_table(table,
                                                 query_uuid=ex["queryUuid"],
                                                 sql=ex.get("perfettoSql"))
            rows = self.duckdb.row_count(table)
        elif table is not None:
            self.duckdb.drop_table(table, ex["queryUuid"])
        with self._lock:
            ex.pop("_ingest", None)
            ex["processedRows"] = rows if materialize else ex["processedRows"]
            ex["tableName"] = table if materialize else None
            if ex["status"] != "CANCELLED":
                ex["status"] = status
            ex["endTime"] = iso_now()
        if deleted:
            self.duckdb.drop_table(table, ex["queryUuid"])
            return
        self._persist(self.duckdb.save_execution, ex)
        self._remember(ex)

    def _update_schema(self, ex: Dict[str, Any],
                       sql_columns: List[str]) -> None:
        with self._lock:
            schema_map = ex["_schema_map"]
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
        """Streams a filtered, sorted and projected page directly from DuckDB."""
        ex = self._load(query_uuid)
        with self._lock:
            table_name = ex.get("tableName") or ""
            all_columns = list(ex.get("_columns") or [])
            schema_map = dict(ex.get("_schema_map") or {})
            visible = [s["name"] for s in ex.get("schema") or []]
            if not all_columns and visible:
                all_columns = list(visible)
            for s in ex.get("schema") or []:
                schema_map.setdefault(s["name"], s.get("type", "STRING"))

        offset, limit = max(0, offset), max(0, limit)
        out_indices = _column_indices(all_columns, columns or visible or
                                      all_columns)
        out_columns = [all_columns[i] for i in out_indices]

        total, page = self.duckdb.query_page(
            table_name=table_name,
            all_columns=all_columns,
            out_indices=out_indices,
            offset=offset,
            limit=limit,
            order_by=order_by,
            filters=filters,
        )

        return {
            "queryUuid": query_uuid,
            "tableName": table_name,
            "schema": [{
                "name": c,
                "type": schema_map.get(c, "STRING")
            } for c in out_columns],
            "rows": [{
                "values": [to_wire(v) for v in r]
            } for r in page],
            "totalFilteredRows": total,
            "availableColumnNames": all_columns,
        }


def _column_indices(all_columns: List[str], wanted: List[str]) -> List[int]:
    used: set = set()
    out = []
    for name in wanted:
        idx = next((i for i, c in enumerate(all_columns)
                    if c == name and i not in used), None)
        if idx is not None:
            used.add(idx)
            out.append(idx)
    return out


_Outcome = Tuple[Optional[FeedResult], Optional[BaseException]]


class _TraceRun:
    """State of one query over traces while it runs."""

    def __init__(self, ex: Dict[str, Any], traces: List[TraceEntry]):
        self.ex = ex
        self.traces = traces
        self.ingest: StreamIngest
        self.errors: List[str] = []
        self.succeeded: Set[int] = set()
        # Traces that failed after queueing rows, which must be deleted.
        self.partial: Set[int] = set()
        # The row limit was reached.
        self.stopped = False
        self.fatal: Optional[str] = None
        # Set once the result table exists.
        self.table: Optional[str] = None
        self.columns: List[str] = []
        self.with_uuid = True

    def wants_more(self) -> bool:
        return not self.stopped and not self.ex["_cancel"].is_set()

    def record(self, lock: Any, trace_ord: int, trace: TraceEntry,
               outcome: _Outcome) -> None:
        res, err = outcome
        with lock:
            ex = self.ex
            ex["processedTraces"] += 1
            cancelled = ex["_cancel"].is_set()
            if err is not None:
                if not cancelled:
                    logger.info("Query failed on %s: %s", trace.file_name, err)
                    self.errors.append(f"{trace.file_name}: {err}")
            elif res is not None:
                if res.ok:
                    self.succeeded.add(trace_ord)
                    ex["processedRows"] += res.rows
                    if res.status == stream_ingest.FEED_LIMIT:
                        self.stopped = True
                else:
                    if res.rows:
                        self.partial.add(trace_ord)
                    if not (res.status == stream_ingest.FEED_CANCELLED and
                            cancelled):
                        msg = res.message or "cancelled"
                        logger.info("Query failed on %s: %s", trace.file_name,
                                    msg)
                        self.errors.append(f"{trace.file_name}: {msg}")
            if 0 < ex["limit"] <= ex["processedRows"]:
                self.stopped = True
