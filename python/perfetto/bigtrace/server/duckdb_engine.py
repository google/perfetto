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
"""DuckDB engine for storing trace query tables, execution history, and paging."""

import datetime
import decimal
import glob
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import threading
from typing import (Any, Callable, Collection, Dict, List, Optional, Sequence,
                    Tuple)

from perfetto.bigtrace.server.catalog import TraceCatalog
from perfetto.bigtrace.server.filters import parse_order_by
from perfetto.bigtrace.server.stream_ingest import TpStreamApi
from perfetto.bigtrace.server.util import iso_now

logger = logging.getLogger(__name__)

_EXT_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "duckdb_ext"))
_EXT_PATH = os.path.join(_EXT_DIR, "perfetto.duckdb_extension")

TERMINAL_STATUSES = ("SUCCESS", "FAILED", "CANCELLED")

STATUS_FIELDS = (
    "queryUuid",
    "status",
    "startTime",
    "endTime",
    "processedRows",
    "processedTraces",
    "totalTraces",
    "error",
    "errorMessage",
)

PUBLIC_FIELDS = STATUS_FIELDS + (
    "perfettoSql",
    "queryEngine",
    "limit",
    "traceLimit",
    "materialized",
    "tableName",
    "tableLink",
    "schema",
    "settings",
    "traceFilters",
    "traceMetadataColumns",
    "traceOrderBy",
    "experimentFilter",
)

_RUNTIME_KEYS = ("_cancel", "_batch_cancel", "_active", "_duck_cursor",
                 "_deleted", "_ingest")

_BIGTRACE_TO_DUCKDB_TYPE = {
    "INT64": "BIGINT",
    "DOUBLE": "DOUBLE",
    "FLOAT64": "DOUBLE",
    "BOOLEAN": "BOOLEAN",
    "TIMESTAMP": "VARCHAR",
    "STRING": "VARCHAR",
}


def public_view(ex: Dict[str, Any],
                fields: Tuple[str, ...] = PUBLIC_FIELDS) -> Dict[str, Any]:
    """Returns the wire form of an execution, omitting unset fields."""
    return {k: ex[k] for k in fields if ex.get(k) not in (None, [], {})}


def _dedup_column_names(columns: Sequence[str]) -> List[str]:
    seen: Dict[str, int] = {}
    unique_cols: List[str] = []
    for c in columns:
        base = c or "col"
        if base in seen:
            seen[base] += 1
            unique_cols.append(f"{base}_{seen[base]}")
        else:
            seen[base] = 0
            unique_cols.append(base)
    return unique_cols


def sanitize_table_name(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", name.strip())
    if not cleaned:
        return ""
    if cleaned[0].isdigit():
        cleaned = "t_" + cleaned
    return cleaned


def _quote_ident(ident: str) -> str:
    return '"' + ident.replace('"', '""') + '"'


def _quote_str(val: str) -> str:
    return "'" + val.replace("'", "''") + "'"


def _load_duckdb_module():
    # Extensions resolve DuckDB symbols from the already loaded module, so it
    # must be loaded RTLD_GLOBAL. Restore the flags for everyone else.
    flags = sys.getdlopenflags()
    if hasattr(os, "RTLD_GLOBAL") and hasattr(os, "RTLD_NOW"):
        sys.setdlopenflags(os.RTLD_GLOBAL | os.RTLD_NOW)
    try:
        import duckdb  # pylint: disable=import-outside-toplevel
    finally:
        sys.setdlopenflags(flags)
    return duckdb


def _extension_is_stale() -> bool:
    if not os.path.isfile(_EXT_PATH):
        return True
    built = os.path.getmtime(_EXT_PATH)
    sources = [os.path.join(_EXT_DIR, "build.sh")]
    for pattern in ("src/*.cc", "src/*.h", "sql/*.sql"):
        sources += glob.glob(os.path.join(_EXT_DIR, pattern))
    return any(
        os.path.getmtime(s) > built for s in sources if os.path.isfile(s))


def _ensure_extension_built() -> str:
    """Returns the extension path, building it first if missing or stale.

  Building needs g++ (duckdb_ext/build.sh). Without it a stale build is used
  as is, and a missing one is an error.
  """
    if not _extension_is_stale():
        return _EXT_PATH
    build_script = os.path.join(_EXT_DIR, "build.sh")
    have_build = os.path.isfile(_EXT_PATH)
    if not os.path.isfile(build_script) or not shutil.which("g++"):
        if have_build:
            logger.warning(
                "perfetto.duckdb_extension is older than its sources "
                "and g++ is unavailable to rebuild it")
            return _EXT_PATH
        raise RuntimeError(
            f"{_EXT_PATH} is missing; build it with {build_script} (needs g++)")
    logger.info("Building perfetto.duckdb_extension")
    try:
        subprocess.run([build_script],
                       cwd=_EXT_DIR,
                       check=True,
                       capture_output=True,
                       text=True,
                       timeout=600)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"Building {_EXT_PATH} failed:\n{e.stderr[-2000:]}")
    except (OSError, subprocess.SubprocessError) as e:
        raise RuntimeError(f"Building {_EXT_PATH} failed: {e}") from e
    return _EXT_PATH


def _cgroup_memory_limit_bytes() -> Optional[int]:
    """The memory limit of this process's cgroup (v2), if any."""
    try:
        with open("/proc/self/cgroup") as f:
            path = next(
                (l.strip().split(":", 2)[2] for l in f if l.startswith("0::")),
                None)
        if path is None:
            return None
        # The tightest limit on the way up to the root applies.
        limits = []
        while True:
            try:
                with open(f"/sys/fs/cgroup{path}/memory.max") as f:
                    val = f.read().strip()
                if val != "max":
                    limits.append(int(val))
            except (OSError, ValueError):
                pass
            if path in ("/", ""):
                break
            path = os.path.dirname(path)
        return min(limits) if limits else None
    except OSError:
        return None


def _default_memory_limit_gb() -> int:
    # DuckDB spills to disk past this. Trace processors (often 100+ at a time,
    # up to GBs each) share the machine, so leave them most of the memory.
    try:
        total = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (ValueError, OSError, AttributeError):
        return 16
    cgroup = _cgroup_memory_limit_bytes()
    if cgroup:
        total = min(total, cgroup)
    return max(2, int((total / (1 << 30)) * 0.40))


def _cleanup_stale_ephemeral_dbs(cache_dir: str) -> None:
    try:
        for fname in os.listdir(cache_dir):
            m = re.match(r"^ephemeral_(\d+)_\d+\.duckdb", fname)
            if not m:
                continue
            pid = int(m.group(1))
            if pid == os.getpid():
                continue
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                try:
                    os.remove(os.path.join(cache_dir, fname))
                except OSError:
                    pass
            except PermissionError:
                pass
    except OSError:
        pass


class DuckDBEngine:
    """Manages the embedded DuckDB instance, query tables, history, and paging."""

    def __init__(self,
                 db_path: Optional[str] = None,
                 parquet_dir: Optional[str] = None):
        """Opens `db_path`, or if None a temporary database file under
    ~/.cache/perfetto/bigtrace/duckdb (deleted on close) rather than an
    in-memory one, so that large results can spill to disk."""
        self.db_path = db_path or ":memory:"
        self._ephemeral_db_path: Optional[str] = None
        if self.db_path == ":memory:":
            cache_dir = os.path.join(
                os.path.expanduser("~/.cache/perfetto/bigtrace"), "duckdb")
            try:
                os.makedirs(cache_dir, exist_ok=True)
                _cleanup_stale_ephemeral_dbs(cache_dir)
                ephem = os.path.join(
                    cache_dir, f"ephemeral_{os.getpid()}_{id(self)}.duckdb")
                self._ephemeral_db_path = ephem
                connect_path = ephem
            except OSError:
                connect_path = ":memory:"
        else:
            self.db_path = os.path.abspath(os.path.expanduser(self.db_path))
            os.makedirs(os.path.dirname(self.db_path), exist_ok=True)
            connect_path = self.db_path
        self.parquet_dir = (os.path.abspath(os.path.expanduser(parquet_dir))
                            if parquet_dir else None)
        if self.parquet_dir:
            os.makedirs(self.parquet_dir, exist_ok=True)
        self._lock = threading.RLock()
        self._counter = 0
        self._table_meta: Dict[str, Dict[str, Any]] = {}
        self._latest_table: Optional[str] = None
        self._catalog_uuids: List[str] = []
        self._catalog_uuid_set: set[str] = set()
        self._duckdb = _load_duckdb_module()
        try:
            self._conn = self._duckdb.connect(
                connect_path, config={"allow_unsigned_extensions": "true"})
        except Exception as e:
            wal_path = f"{connect_path}.wal"
            if connect_path != ":memory:" and os.path.exists(wal_path):
                logger.warning("Removing unreplayable DuckDB WAL %s (%s)",
                               wal_path, e)
                try:
                    os.remove(wal_path)
                except OSError:
                    pass
                self._conn = self._duckdb.connect(
                    connect_path, config={"allow_unsigned_extensions": "true"})
            else:
                raise
        self._conn.execute("SET preserve_insertion_order = false")
        self._conn.execute(
            f"SET memory_limit = '{_default_memory_limit_gb()}GB'")
        ext_path = _ensure_extension_built()
        self._conn.execute(f"LOAD {_quote_str(ext_path)}")
        self.extension_loaded = True
        # Streams trace query results into tables; see stream_ingest.py.
        self.stream_api: Optional[TpStreamApi] = TpStreamApi(ext_path)
        self._init_history_schema()
        if self.parquet_dir:
            self._load_existing_parquet()
        # User DuckDB queries must not reach the file system (they come from any
        # page that can reach the server): only Parquet exports are allowed, and
        # the configuration is locked so queries cannot undo this.
        allowed = [self.parquet_dir] if self.parquet_dir else []
        self._conn.execute("SET allowed_directories = ?", [allowed])
        self._conn.execute("SET enable_external_access = false")
        self._conn.execute("PRAGMA disable_print_progress_bar")
        self._exec_cursors: List[Any] = []
        for _ in range(8):
            c = self._conn.cursor()
            c.execute("PRAGMA enable_progress_bar; "
                      "PRAGMA disable_print_progress_bar; "
                      "SET progress_bar_time = 0;")
            self._exec_cursors.append(c)
        self._conn.execute("SET lock_configuration = true")

    def _init_history_schema(self) -> None:
        with self._lock:
            self._conn.execute("""
          CREATE TABLE IF NOT EXISTS _bigtrace_executions (
            query_uuid VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            start_time VARCHAR,
            data VARCHAR NOT NULL
          )""")
            stale = self._conn.execute(
                "SELECT data FROM _bigtrace_executions WHERE status = 'IN_PROGRESS'"
            ).fetchall()
            for (data,) in stale:
                ex = json.loads(data)
                ex.update(status="CANCELLED",
                          endTime=iso_now(),
                          errorMessage="Interrupted by server restart")
                self.save_execution(ex)
            for row in self.list_executions():
                tname = row.get("tableName")
                uid = row.get("queryUuid")
                if tname and not tname.startswith("_bigtrace"):
                    m = re.fullmatch(r"result_(\d+)", tname)
                    if m:
                        self._counter = max(self._counter, int(m.group(1)))
                    if self._physical_table_exists_locked(tname):
                        self._record_table_meta_locked(
                            tname, query_uuid=uid, sql=row.get("perfettoSql"))

    def _physical_table_exists_locked(self, table_name: str) -> bool:
        row = self._conn.execute(
            "SELECT 1 FROM information_schema.tables "
            "WHERE table_schema = 'main' AND table_name = ?",
            [table_name]).fetchone()
        return row is not None

    def _load_existing_parquet(self) -> None:
        if not self.parquet_dir or not os.path.isdir(self.parquet_dir):
            return
        with self._lock:
            for fname in sorted(os.listdir(self.parquet_dir)):
                if not fname.endswith(".parquet") or fname.startswith("."):
                    continue
                tname = sanitize_table_name(fname[:-len(".parquet")])
                if not tname or tname.startswith("_bigtrace"):
                    continue
                fpath = os.path.join(self.parquet_dir, fname)
                try:
                    self._conn.execute(
                        f"CREATE OR REPLACE TABLE {_quote_ident(tname)} "
                        f"AS SELECT * FROM read_parquet({_quote_str(fpath)})")
                    self._record_table_meta_locked(tname)
                    m = re.fullmatch(r"result_(\d+)", tname)
                    if m:
                        self._counter = max(self._counter, int(m.group(1)))
                except self._duckdb.Error as e:
                    logger.warning("Could not load parquet %s: %s", fpath, e)

    def _checkpoint_locked(self) -> None:
        if self.db_path == ":memory:":
            return
        try:
            self._conn.execute("CHECKPOINT")
        except self._duckdb.Error:
            pass

    # Execution metadata persistence in DuckDB --------------------------------

    def save_execution(self, ex: Dict[str, Any]) -> None:
        doc = {k: v for k, v in ex.items() if k not in _RUNTIME_KEYS}
        with self._lock:
            self._conn.execute(
                "INSERT OR REPLACE INTO _bigtrace_executions VALUES (?, ?, ?, ?)",
                [
                    doc["queryUuid"],
                    doc.get("status", "UNKNOWN"),
                    doc.get("startTime"),
                    json.dumps(doc, default=str),
                ])
            if doc.get("status") != "IN_PROGRESS":
                self._checkpoint_locked()

    def get_execution(self, query_uuid: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            row = self._conn.execute(
                "SELECT data FROM _bigtrace_executions WHERE query_uuid = ?",
                [query_uuid]).fetchone()
        return json.loads(row[0]) if row else None

    def list_executions(self) -> List[Dict[str, Any]]:
        with self._lock:
            rows = self._conn.execute("SELECT data FROM _bigtrace_executions "
                                      "ORDER BY start_time DESC").fetchall()
        return [public_view(json.loads(data)) for (data,) in rows]

    def get_status(self, query_uuid: str) -> Optional[Dict[str, Any]]:
        ex = self.get_execution(query_uuid)
        return public_view(ex, STATUS_FIELDS) if ex else None

    def mark_cancelled(self, query_uuid: str) -> None:
        ex = self.get_execution(query_uuid)
        if ex and ex.get("status") == "IN_PROGRESS":
            ex.update(status="CANCELLED", endTime=iso_now())
            self.save_execution(ex)

    def delete_execution_record(self, query_uuid: str) -> bool:
        with self._lock:
            exists = self._conn.execute(
                "SELECT 1 FROM _bigtrace_executions WHERE query_uuid = ?",
                [query_uuid]).fetchone()
            if exists:
                self._conn.execute(
                    "DELETE FROM _bigtrace_executions WHERE query_uuid = ?",
                    [query_uuid])
                self._checkpoint_locked()
            return exists is not None

    # Catalog & Table naming --------------------------------------------------

    def sync_catalog(self, catalog: TraceCatalog) -> None:
        """Materializes `trace_metadata` with shared `_trace_uuid` ENUM type."""
        traces = catalog.get_traces()
        schema = catalog.get_schema()
        col_names = ["_trace_uuid"] + [
            c["name"] for c in schema if c["name"] != "_trace_uuid"
        ]
        col_types = {c["name"]: c["type"] for c in schema}
        col_types["_trace_uuid"] = "STRING"
        uuids = [t.trace_uuid for t in traces]
        col_data: List[List[Any]] = [uuids]
        for c in col_names[1:]:
            col_data.append([t.get_value(c) for t in traces])
        with self._lock:
            domain = list(dict.fromkeys(uuids))
            if 0 < len(domain) <= 65535 and domain != self._catalog_uuids:
                try:
                    self._conn.execute('DROP TYPE IF EXISTS "_bt_trace_uuid"')
                    vals_sql = ", ".join(_quote_str(u) for u in domain)
                    self._conn.execute(
                        f'CREATE TYPE "_bt_trace_uuid" AS ENUM ({vals_sql})')
                    self._catalog_uuids = domain
                    self._catalog_uuid_set = set(domain)
                except self._duckdb.Error as e:
                    # A persisted result table still uses the existing ENUM type: reuse it.
                    try:
                        existing = [
                            r[0] for r in self._conn.execute(
                                'SELECT unnest(enum_range(NULL::"_bt_trace_uuid"))::VARCHAR'
                            ).fetchall()
                        ]
                        self._catalog_uuids = existing
                        self._catalog_uuid_set = set(existing)
                    except self._duckdb.Error:
                        logger.warning(
                            "Could not update the trace UUID type: %s", e)
            self.create_streaming_table("trace_metadata", col_names, col_types)
            self._insert_metadata(col_names, col_types, col_data)
            self.finalize_streaming_table("trace_metadata",
                                          sql="-- Trace catalog metadata",
                                          export=False)

    def next_table_name(self,
                        preferred: Optional[str] = None,
                        reserved: Optional[Collection[str]] = None) -> str:
        with self._lock:
            reserved_set = set(reserved or ())
            if preferred:
                cleaned = sanitize_table_name(preferred)
                if cleaned:
                    candidate = cleaned
                    idx = 2
                    while candidate in reserved_set:
                        candidate = f"{cleaned}_{idx}"
                        idx += 1
                    return candidate
            while True:
                self._counter += 1
                candidate = f"result_{self._counter}"
                if (candidate not in self._table_meta and
                        candidate not in reserved_set and
                        not self._physical_table_exists_locked(candidate)):
                    return candidate

    def suggest_table_name(self,
                           base_name: str,
                           reserved: Optional[Collection[str]] = None) -> str:
        cleaned = sanitize_table_name(base_name) or "tbl"
        with self._lock:
            reserved_set = set(reserved or ())
            idx = 1
            while True:
                candidate = f"{cleaned}_{idx}"
                if (candidate not in self._table_meta and
                        candidate not in reserved_set and
                        not self._physical_table_exists_locked(candidate)):
                    return candidate
                idx += 1

    def table_exists(self, table_name: str) -> Tuple[bool, str]:
        resolved = sanitize_table_name(table_name)
        if not resolved or resolved.startswith("_bigtrace"):
            return False, ""
        with self._lock:
            if resolved in self._table_meta or self._physical_table_exists_locked(
                    resolved):
                return True, resolved
            return False, resolved

    # Direct streaming into DuckDB tables -------------------------------------

    def create_streaming_table(self,
                               table_name: str,
                               columns: Sequence[str],
                               schema_map: Dict[str, str],
                               query_uuid: Optional[str] = None,
                               sql: Optional[str] = None,
                               uuid_enum: bool = True) -> Tuple[str, List[str]]:
        """Creates or replaces an empty DuckDB table ready for streaming appends.

    `_trace_uuid` gets the shared `_bt_trace_uuid` ENUM type if `uuid_enum`
    and the catalog defines it.
    """
        resolved = sanitize_table_name(table_name) or self.next_table_name()
        col_names = list(columns) or ["_trace_uuid"]
        unique_cols = _dedup_column_names(col_names)
        col_defs = []
        col_meta = []
        for orig, uniq in zip(col_names, unique_cols):
            btype = schema_map.get(orig, "STRING")
            if orig == "_trace_uuid" and uuid_enum and self._catalog_uuids:
                dtype = '"_bt_trace_uuid"'
                col_defs.append(f"{_quote_ident(uniq)} {dtype}")
                col_meta.append({"name": uniq, "type": "STRING"})
            else:
                dtype = _BIGTRACE_TO_DUCKDB_TYPE.get(btype, "VARCHAR")
                col_defs.append(f"{_quote_ident(uniq)} {dtype}")
                col_meta.append({
                    "name": uniq,
                    "type": _duckdb_type_to_bigtrace(dtype)
                })

        qtable = _quote_ident(resolved)
        with self._lock:
            m = re.fullmatch(r"result_(\d+)", resolved)
            if m:
                self._counter = max(self._counter, int(m.group(1)))
            try:
                self._conn.execute(f"DROP VIEW IF EXISTS {qtable}")
            except self._duckdb.CatalogException:
                pass  # A table of that name, which is replaced below.
            self._conn.execute(
                f"CREATE OR REPLACE TABLE {qtable} ({', '.join(col_defs)})")
            prev = self._table_meta.get(resolved, {})
            self._table_meta[resolved] = {
                "name": resolved,
                "rowCount": 0,
                "columns": col_meta,
                "queryUuid": query_uuid or prev.get("queryUuid"),
                "sql": sql or prev.get("sql"),
            }
            self._latest_table = resolved
        return resolved, unique_cols

    def _insert_metadata(self, columns: Sequence[str], schema_map: Dict[str,
                                                                        str],
                         col_data: Sequence[Sequence[Any]]) -> None:
        """Fills trace_metadata, typed as created by create_streaming_table."""
        if not col_data or not col_data[0]:
            return
        types = [schema_map.get(c, "STRING") for c in columns]
        rows = [
            tuple(_coerce_metadata_cell(v, t)
                  for v, t in zip(row, types))
            for row in zip(*col_data)
        ]
        placeholders = ", ".join(["?"] * len(columns))
        with self._lock:
            self._conn.executemany(
                f'INSERT INTO "trace_metadata" VALUES ({placeholders})', rows)

    def uuid_enum_covers(self, uuids: Sequence[str]) -> bool:
        """Whether all `uuids` are values of the `_bt_trace_uuid` ENUM."""
        with self._lock:
            return bool(self._catalog_uuid_set and
                        all(u in self._catalog_uuid_set for u in uuids))

    def create_result_table(self, table_name: str, columns: Sequence[str],
                            types: Sequence[str], with_uuid: bool,
                            uuid_enum: bool, ord_column: Optional[str],
                            query_uuid: str, sql: str) -> Tuple[str, List[str]]:
        """Creates the empty table a query over traces streams into.

    Columns: the query's, then `_trace_uuid` if `with_uuid`, then
    `ord_column` (INTEGER) if set. Returns the table name and the unique
    names of the query's columns.
    """
        names = list(columns) + (["_trace_uuid"] if with_uuid else [])
        schema = dict(zip(columns, types))
        if with_uuid:
            schema["_trace_uuid"] = "STRING"
        table, unique = self.create_streaming_table(table_name,
                                                    names,
                                                    schema,
                                                    query_uuid=query_uuid,
                                                    sql=sql,
                                                    uuid_enum=uuid_enum)
        if ord_column:
            with self._lock:
                self._conn.execute(
                    f"ALTER TABLE {_quote_ident(table)} "
                    f"ADD COLUMN {_quote_ident(ord_column)} INTEGER")
        return table, unique[:len(columns)]

    def insert_from_stream(self, table_name: str, handle: int,
                           names: Sequence[str], types: Sequence[str],
                           uuids: Optional[Sequence[str]], uuid_enum: bool,
                           ord_column: Optional[str], scan_threads: int) -> int:
        """Drains stream `handle` into `table_name`; returns the rows inserted.

    Runs on its own cursor so it does not block other DuckDB calls. Rows get
    `_trace_uuid` = uuids[ord] if `uuids` is set, and the trace ord in
    `ord_column` if set; see create_result_table.
    """
        qtable = _quote_ident(sanitize_table_name(table_name))
        proj = [f"p.{_quote_ident(n)}" for n in names]
        if uuids is not None:
            utype = '"_bt_trace_uuid"' if uuid_enum else "VARCHAR"
            proj.append(f'p."_trace_uuid"::{utype}')
        if ord_column:
            proj.append('p."_trace_ord"')
        params = [
            handle, scan_threads,
            list(names),
            list(types),
            list(uuids or [])
        ]
        with self._lock:
            cur = self._conn.cursor()
        try:
            row = cur.execute(
                f"INSERT INTO {qtable} SELECT {', '.join(proj)} FROM "
                "pf_scan_tp_stream(?, ?, ?::VARCHAR[], ?::VARCHAR[], ?::VARCHAR[]) p",
                params).fetchone()
            return int(row[0]) if row else 0
        finally:
            cur.close()

    def delete_trace_rows(self,
                          table_name: str,
                          uuids: Optional[Sequence[str]] = None,
                          ords: Optional[Sequence[int]] = None,
                          ord_column: Optional[str] = None) -> None:
        """Deletes the rows of traces whose results failed part way.

    Traces are identified by `_trace_uuid`, or by trace ord in `ord_column`.
    """
        qtable = _quote_ident(sanitize_table_name(table_name))
        with self._lock:
            if uuids:
                self._conn.execute(
                    f'DELETE FROM {qtable} WHERE "_trace_uuid"::VARCHAR '
                    "IN (SELECT unnest(?::VARCHAR[]))", [list(uuids)])
            if ords and ord_column:
                self._conn.execute(
                    f"DELETE FROM {qtable} WHERE {_quote_ident(ord_column)} "
                    "IN (SELECT unnest(?::INTEGER[]))", [list(ords)])

    def drop_column(self, table_name: str, column: str) -> None:
        with self._lock:
            self._conn.execute(
                f"ALTER TABLE {_quote_ident(sanitize_table_name(table_name))} "
                f"DROP COLUMN {_quote_ident(column)}")

    def row_count(self, table_name: str) -> int:
        """Rows of `table_name` as of its last finalize_streaming_table()."""
        with self._lock:
            meta = self._table_meta.get(sanitize_table_name(table_name)) or {}
            return int(meta.get("rowCount", 0))

    def retype_columns(self, table_name: str, types: Dict[str, str]) -> None:
        """Converts VARCHAR columns that only hold numbers to `types`."""
        qtable = _quote_ident(sanitize_table_name(table_name))
        with self._lock:
            for col, btype in types.items():
                dtype = _BIGTRACE_TO_DUCKDB_TYPE.get(btype, "VARCHAR")
                qcol = _quote_ident(col)
                self._conn.execute(
                    f"ALTER TABLE {qtable} ALTER {qcol} TYPE {dtype} "
                    f"USING TRY_CAST({qcol} AS {dtype})")

    def finalize_streaming_table(self,
                                 table_name: str,
                                 query_uuid: Optional[str] = None,
                                 sql: Optional[str] = None,
                                 export: bool = True) -> None:
        resolved = sanitize_table_name(table_name)
        if not resolved:
            return
        with self._lock:
            if not self._physical_table_exists_locked(resolved):
                return
            if self._latest_table == resolved and resolved != "trace_metadata":
                self._conn.execute(
                    f"CREATE OR REPLACE VIEW latest AS SELECT * FROM {_quote_ident(resolved)}"
                )
            if query_uuid:
                alias = f"bigtrace_{query_uuid.replace('-', '_')}"
                if alias != resolved:
                    self._conn.execute(
                        f"CREATE OR REPLACE VIEW {_quote_ident(alias)} "
                        f"AS SELECT * FROM {_quote_ident(resolved)}")
            if export and self.parquet_dir:
                pq_path = os.path.join(self.parquet_dir, f"{resolved}.parquet")
                tmp_path = os.path.join(self.parquet_dir,
                                        f".{resolved}.parquet.tmp")
                try:
                    self._conn.execute(
                        f"COPY {_quote_ident(resolved)} TO {_quote_str(tmp_path)} "
                        "(FORMAT PARQUET, COMPRESSION ZSTD)")
                    os.replace(tmp_path, pq_path)
                except (self._duckdb.Error, OSError) as e:
                    logger.warning("Parquet export for %s failed: %s", resolved,
                                   e)
                    try:
                        os.remove(tmp_path)
                    except OSError:
                        pass
            self._record_table_meta_locked(resolved,
                                           query_uuid=query_uuid,
                                           sql=sql)
            self._checkpoint_locked()

    def _record_table_meta_locked(self,
                                  table_name: str,
                                  query_uuid: Optional[str] = None,
                                  sql: Optional[str] = None) -> None:
        qtable = _quote_ident(table_name)
        count = self._conn.execute(
            f"SELECT count(*) FROM {qtable}").fetchone()[0]
        desc = self._conn.execute(f"DESCRIBE {qtable}").fetchall()
        cols = [{
            "name": r[0],
            "type": _duckdb_type_to_bigtrace(str(r[1]))
        } for r in desc]
        prev = self._table_meta.get(table_name, {})
        self._table_meta[table_name] = {
            "name": table_name,
            "rowCount": int(count),
            "columns": cols,
            "queryUuid": query_uuid or prev.get("queryUuid"),
            "sql": sql or prev.get("sql"),
        }

    def drop_table(self,
                   table_name: Optional[str],
                   query_uuid: Optional[str] = None) -> None:
        if not table_name:
            return
        resolved = sanitize_table_name(table_name)
        with self._lock:
            owner = self._table_meta.get(resolved, {}).get("queryUuid")
            if query_uuid and owner and owner != query_uuid:
                resolved = ""
            if resolved:
                try:
                    self._conn.execute(
                        f"DROP TABLE IF EXISTS {_quote_ident(resolved)}")
                except self._duckdb.Error as e:
                    logger.warning("Could not drop table %s: %s", resolved, e)
                self._table_meta.pop(resolved, None)
            if query_uuid:
                alias = f"bigtrace_{query_uuid.replace('-', '_')}"
                try:
                    self._conn.execute(
                        f"DROP VIEW IF EXISTS {_quote_ident(alias)}")
                except self._duckdb.Error as e:
                    logger.warning("Could not drop view %s: %s", alias, e)
            if self.parquet_dir and resolved:
                pq_path = os.path.join(self.parquet_dir, f"{resolved}.parquet")
                try:
                    os.remove(pq_path)
                except OSError:
                    pass
            self._checkpoint_locked()

    def list_tables(self) -> List[Dict[str, Any]]:
        with self._lock:
            return [
                dict(meta)
                for name, meta in self._table_meta.items()
                if not name.startswith("_bigtrace")
            ]

    def execute_into_table(
        self,
        sql: str,
        table_name: str,
        limit: int,
        query_uuid: Optional[str] = None,
        on_cursor: Optional[Callable[[Any], None]] = None,
    ) -> Tuple[List[str], Dict[str, str], int]:
        """Runs DuckDB SQL natively in C++ and materializes directly into `table_name`."""
        resolved = sanitize_table_name(table_name) or self.next_table_name()
        qtable = _quote_ident(resolved)
        clean_sql = sql.strip().rstrip(";").strip()
        with self._lock:
            m = re.fullmatch(r"result_(\d+)", resolved)
            if m:
                self._counter = max(self._counter, int(m.group(1)))
            try:
                self._conn.execute(f"DROP VIEW IF EXISTS {qtable}")
            except self._duckdb.CatalogException:
                pass  # A table of that name, which is replaced below.
            from_pool = bool(self._exec_cursors)
            cur = (self._exec_cursors.pop()
                   if from_pool else self._conn.cursor())
        try:
            if not from_pool:
                try:
                    cur.execute("SET enable_progress_bar=true; "
                                "SET enable_progress_bar_print=false; "
                                "SET progress_bar_time=0;")
                except self._duckdb.Error:
                    pass
            if on_cursor:
                on_cursor(cur)
            compiled_sql = sql
            if self.extension_loaded:
                try:
                    row = cur.execute("SELECT pf_compile_pipe(?)",
                                      [sql]).fetchone()
                    if row and row[0]:
                        compiled_sql = str(row[0])
                except self._duckdb.Error:
                    compiled_sql = sql
            clean_sql = compiled_sql.strip().rstrip(";").strip()
            # The query runs as a plain read-only SELECT on its own cursor so that
            # interrupt() is immediate and concurrent table writes never block on
            # it. LIMIT is pushed into the query; only the output rows are streamed
            # (as Arrow batches) into the destination table.
            limit_sql = f" LIMIT {int(limit)}" if limit > 0 else ""
            try:
                cur.execute(f"SELECT * FROM ({clean_sql}) AS _bt_q WHERE 1 = 0")
                wrappable = True
            except self._duckdb.Error:
                wrappable = False  # E.g. PRAGMA / SHOW / multi-statement scripts.
                cur.execute(compiled_sql)
            if not cur.description:
                return [], {}, 0
            description = cur.description
            raw_columns = [d[0] for d in description]
            unique_cols = _dedup_column_names(raw_columns)
            schema_map = {
                u: _duckdb_type_to_bigtrace(str(d[1]))
                for u, d in zip(unique_cols, description)
            }
            select_exprs = []
            for orig, uniq, desc in zip(raw_columns, unique_cols, description):
                dtype = str(desc[1]).upper()
                qorig, quniq = _quote_ident(orig), _quote_ident(uniq)
                if any(t in dtype
                       for t in ("TIMESTAMP", "DATE", "TIME", "INTERVAL")):
                    select_exprs.append(f"CAST({qorig} AS VARCHAR) AS {quniq}")
                elif orig != uniq:
                    select_exprs.append(f"{qorig} AS {quniq}")
                else:
                    select_exprs.append(qorig)
            select_cols = ", ".join(select_exprs)
            if self.stream_api is not None:
                self.stream_api.reset_query_progress()
            if wrappable:
                cur.execute(f"CREATE OR REPLACE TABLE {qtable} AS "
                            f"SELECT {select_cols} FROM ({clean_sql}) AS _bt_q"
                            f"{limit_sql}")
            else:
                rows = cur.fetchmany(limit) if limit > 0 else cur.fetchall()
                self.create_streaming_table(resolved,
                                            unique_cols,
                                            schema_map,
                                            query_uuid=query_uuid,
                                            sql=sql,
                                            uuid_enum=False)
                if rows:
                    placeholders = ", ".join(["?"] * len(unique_cols))
                    cur.executemany(
                        f"INSERT INTO {qtable} VALUES ({placeholders})",
                        [[_to_cell(v) for v in r] for r in rows])
            with self._lock:
                self._latest_table = resolved
            self.finalize_streaming_table(resolved,
                                          query_uuid=query_uuid,
                                          sql=sql)
            total_rows = int(
                self._table_meta.get(resolved, {}).get("rowCount", 0))
        finally:
            if self.stream_api is not None:
                self.stream_api.reset_query_progress()
            if from_pool:
                with self._lock:
                    self._exec_cursors.append(cur)
            else:
                cur.close()
        return unique_cols, schema_map, total_rows

    def query_page(
        self,
        table_name: str,
        all_columns: Sequence[str],
        out_indices: Sequence[int],
        offset: int = 0,
        limit: int = 50,
        order_by: Optional[str] = None,
        filters: Optional[List[Dict[str, Any]]] = None,
    ) -> Tuple[int, List[List[Any]]]:
        """Streams a filtered, sorted, projected page directly from DuckDB."""
        resolved = sanitize_table_name(table_name)
        unique_cols = _dedup_column_names(all_columns)
        if not resolved or not unique_cols:
            return 0, []
        with self._lock:
            if not self._physical_table_exists_locked(resolved):
                return 0, []
            cur = self._conn.cursor()
        try:
            qtable = _quote_ident(resolved)
            phys_cols = {
                r[0] for r in cur.execute(f"DESCRIBE {qtable}").fetchall()
            }
            has_meta = (resolved != "trace_metadata" and
                        self._physical_table_exists_locked("trace_metadata"))
            meta_cols = ({
                r[0]
                for r in cur.execute('DESCRIBE "trace_metadata"').fetchall()
            } if has_meta else set())
            col_set = set(unique_cols)
            orig_to_uniq: Dict[str, str] = {}
            for orig, uniq in zip(all_columns, unique_cols):
                orig_to_uniq.setdefault(orig, uniq)
                orig_to_uniq[uniq] = uniq

            needs_meta_join = False

            def _col_expr(uniq: str) -> str:
                nonlocal needs_meta_join
                if uniq in phys_cols:
                    return f"_bt_t.{_quote_ident(uniq)}"
                if uniq in meta_cols:
                    needs_meta_join = True
                    return f"_bt_tm.{_quote_ident(uniq)}"
                return "NULL"

            where_parts: List[str] = []
            params: List[Any] = []
            for f in filters or ():
                field = f.get("field")
                if not field:
                    continue
                op = str(f.get("op", "=")).lower()
                target = f.get("value")
                col_ident = (_col_expr(orig_to_uniq[field])
                             if field in orig_to_uniq else "NULL")
                cond, cond_params = _build_filter_sql(col_ident, op, target)
                where_parts.append(cond)
                params.extend(cond_params)

            order_parts: List[str] = []
            for field, desc in parse_order_by(order_by):
                uniq = orig_to_uniq.get(field)
                if not uniq or uniq not in col_set:
                    continue
                c = _col_expr(uniq)
                direction = "DESC" if desc else "ASC"
                is_empty = (f"({c} IS NULL OR CAST({c} AS VARCHAR) = '' "
                            f"OR UPPER(CAST({c} AS VARCHAR)) = 'NULL')")
                num_val = f"TRY_CAST({c} AS DOUBLE)"
                if desc:
                    bucket = (f"CASE WHEN {is_empty} THEN 0 "
                              f"WHEN {num_val} IS NOT NULL THEN 2 ELSE 1 END")
                else:
                    bucket = (f"CASE WHEN {is_empty} THEN 2 "
                              f"WHEN {num_val} IS NOT NULL THEN 0 ELSE 1 END")
                order_parts.append(f"{bucket} ASC")
                order_parts.append(f"{num_val} {direction}")
                order_parts.append(f"LOWER(CAST({c} AS VARCHAR)) {direction}")
            if order_parts:
                order_parts.append("_bt_t.rowid ASC")
                order_sql = f" ORDER BY {', '.join(order_parts)}"
            else:
                order_sql = ""

            proj = ", ".join(f"{_col_expr(unique_cols[i])} AS c_{k}"
                             for k, i in enumerate(out_indices))
            if not proj:
                proj = "1"

            from_base = f"{qtable} AS _bt_t"
            join_sql = (' LEFT JOIN "trace_metadata" AS _bt_tm '
                        'ON _bt_t."_trace_uuid" = _bt_tm."_trace_uuid"'
                        if needs_meta_join else "")
            where_sql = f" WHERE {' AND '.join(where_parts)}" if where_parts else ""
            count_from = f"{from_base}{join_sql}" if where_parts else from_base
            total = cur.execute(f"SELECT count(*) FROM {count_from}{where_sql}",
                                params).fetchone()[0]
            if limit <= 0 or offset >= total:
                return int(total), []

            sql = (
                f"SELECT {proj} FROM {from_base}{join_sql}{where_sql}{order_sql} "
                f"LIMIT {int(limit)} OFFSET {int(offset)}")
            rows = cur.execute(sql, params).fetchall()
            return int(total), [
                [_to_cell(v) for v in r] if out_indices else [] for r in rows
            ]
        finally:
            cur.close()

    def close(self) -> None:
        with self._lock:
            for c in self._exec_cursors:
                try:
                    c.close()
                except Exception:  # pylint: disable=broad-except
                    pass
            self._exec_cursors.clear()
            self._conn.close()
            if self._ephemeral_db_path:
                for suffix in ("", ".wal"):
                    try:
                        os.remove(self._ephemeral_db_path + suffix)
                    except OSError:
                        pass


_SUPPORTED_FILTER_OPS = {
    "=", "!=", "<", "<=", ">", ">=", "glob", "not glob", "in", "not in",
    "is null", "is not null"
}


def _sql_equals_expr(col_ident: str, target: Any) -> Tuple[str, List[Any]]:
    s_target = "" if target is None else str(target)
    try:
        f_target = float(target)
        return (f"(LOWER(CAST({col_ident} AS VARCHAR)) = LOWER(?) OR "
                f"TRY_CAST({col_ident} AS DOUBLE) = ?)", [s_target, f_target])
    except (TypeError, ValueError):
        return f"LOWER(CAST({col_ident} AS VARCHAR)) = LOWER(?)", [s_target]


def _build_filter_sql(col_ident: str, op: str,
                      target: Any) -> Tuple[str, List[Any]]:
    if op not in _SUPPORTED_FILTER_OPS:
        raise ValueError(f"Unsupported filter op: {op}")
    is_empty = (f"({col_ident} IS NULL OR CAST({col_ident} AS VARCHAR) = '' "
                f"OR UPPER(CAST({col_ident} AS VARCHAR)) = 'NULL')")
    if op == "is null":
        return is_empty, []
    if op == "is not null":
        return f"NOT {is_empty}", []
    if col_ident == "NULL":
        return "FALSE", []
    if op == "=":
        eq, p = _sql_equals_expr(col_ident, target)
        return f"({col_ident} IS NOT NULL AND {eq})", p
    if op == "!=":
        eq, p = _sql_equals_expr(col_ident, target)
        return f"({col_ident} IS NOT NULL AND NOT {eq})", p
    if op in ("<", "<=", ">", ">="):
        s_target = "" if target is None else str(target)
        try:
            f_target = float(target)
            return (
                f"({col_ident} IS NOT NULL AND CASE "
                f"WHEN TRY_CAST({col_ident} AS DOUBLE) IS NOT NULL "
                f"THEN TRY_CAST({col_ident} AS DOUBLE) {op} ? "
                f"ELSE CAST({col_ident} AS VARCHAR) {op} ? END)",
                [f_target, s_target],
            )
        except (TypeError, ValueError):
            return f"({col_ident} IS NOT NULL AND CAST({col_ident} AS VARCHAR) {op} ?)", [
                s_target
            ]
    if op in ("glob", "not glob"):
        pattern = "" if target is None else str(target)
        neg = "NOT " if op == "not glob" else ""
        return f"({col_ident} IS NOT NULL AND {neg}(CAST({col_ident} AS VARCHAR) GLOB ?))", [
            pattern
        ]
    targets = target if isinstance(target, (list, tuple)) else [target]
    if not targets:
        return ("FALSE" if op == "in" else f"({col_ident} IS NOT NULL)"), []
    parts = []
    params: List[Any] = []
    for t in targets:
        eq, p = _sql_equals_expr(col_ident, t)
        parts.append(eq)
        params.extend(p)
    joined = " OR ".join(parts)
    if op == "in":
        return f"({col_ident} IS NOT NULL AND ({joined}))", params
    return f"({col_ident} IS NOT NULL AND NOT ({joined}))", params


def _duckdb_type_to_bigtrace(dtype: str) -> str:
    u = dtype.upper()
    if "INT" in u and "INTERVAL" not in u and "[" not in u and "(" not in u:
        return "INT64"
    if u.startswith(("DOUBLE", "FLOAT", "DECIMAL", "REAL", "NUMERIC")):
        return "DOUBLE"
    if u == "BOOLEAN":
        return "BOOLEAN"
    return "STRING"


def _to_cell(v: Any) -> Any:
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    if isinstance(v, (bytes, bytearray, memoryview)):
        import base64  # pylint: disable=import-outside-toplevel
        return base64.b64encode(bytes(v)).decode("ascii")
    if isinstance(v, decimal.Decimal):
        return float(v)
    if isinstance(v, (datetime.datetime, datetime.date, datetime.time)):
        return v.isoformat()
    if isinstance(v, (list, tuple, dict)):
        return json.dumps(v, default=str)
    return str(v)


def _coerce_metadata_cell(value: Any, btype: str) -> Any:
    if value is None:
        return None
    try:
        if btype == "INT64":
            return int(value)
        if btype in ("DOUBLE", "FLOAT64"):
            return float(value)
        if btype == "BOOLEAN":
            if isinstance(value, str):
                return value.lower() in ("true", "1")
            return bool(value)
        return str(value)
    except (TypeError, ValueError):
        return None
