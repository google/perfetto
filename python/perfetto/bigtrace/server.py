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
"""Local Bigtrace server: the Bigtrace HTTP API over traces on local disk.

A trace query runs on one `trace_processor_shell --stdiod` per trace, in
parallel, and its rows stream into a DuckDB table through the duckdb_ext
extension. DuckDB SQL runs over those tables and `trace_metadata`. See
docs/design-docs/bigtrace-local.md.
"""

import base64
import contextlib
import ctypes
import datetime
import glob
import hashlib
import http.server
import itertools
import json
import logging
import os
import re
import shutil
import sqlite3
import subprocess
import tempfile
import threading
import urllib.parse
import uuid
from concurrent.futures import ThreadPoolExecutor

import duckdb
import pyarrow as pa
import pyarrow.csv

EXT_DIR = os.path.join(os.path.dirname(__file__), "duckdb_ext")
CACHE_DIR = os.path.expanduser("~/.cache/perfetto/bigtrace")
# Hidden files, manifests and DuckDB databases are not traces.
NOT_TRACE = re.compile(r"^\.|manifest\.(json|csv)$|^metadata\.db$|\.duckdb")
# pf_feed() results, see perfetto_ext.cc.
FEED_OK, FEED_ERROR, FEED_CANCELLED, FEED_LIMIT, FEED_EXITED = range(5)
# Result column types and the cell types (bit 1 << CellType) they hold.
HOLDS = {"BIGINT": 0b110, "DOUBLE": 0b1110, "VARCHAR": 0b111110}
STATUS = ("queryUuid", "status", "startTime", "endTime", "tableName",
          "processedRows", "processedTraces", "totalTraces", "errorMessage")


class HttpError(Exception):

  def __init__(self, status, detail):
    super().__init__(detail)
    self.status = status


def load_extension(db):
  """Builds the extension of the current sources, once, and loads it."""
  srcs = sorted(glob.glob(f"{EXT_DIR}/src/*")) + [f"{EXT_DIR}/build.sh"]
  h = hashlib.sha1()
  for s in srcs:
    with open(s, "rb") as f:
      h.update(f.read())
  path = f"{CACHE_DIR}/duckdb_ext/{h.hexdigest()[:16]}/perfetto.duckdb_extension"
  if not os.path.exists(path):
    env = dict(os.environ, DUCKDB_SRC=f"{CACHE_DIR}/duckdb_ext")
    build = subprocess.run(["bash", srcs[-1], path],
                           env=env,
                           capture_output=True,
                           text=True)
    if build.returncode:
      raise SystemExit(f"Building the DuckDB extension failed:\n{build.stderr}")
  db.execute(f"LOAD '{path}'")
  return ctypes.CDLL(path)  # The library DuckDB loaded: they share streams.


def q(ident):
  """Quotes a SQL identifier."""
  return '"' + ident.replace('"', '""') + '"'


def now():
  return datetime.datetime.now(datetime.timezone.utc).isoformat()


def camel(d):
  return {re.sub(r"_(\w)", lambda m: m[1].upper(), k): v for k, v in d.items()}


def wire(v):
  """A cell as the UI wants it: a string, or null."""
  if v is None or isinstance(v, str):
    return v
  if isinstance(v, bool):
    return str(v).lower()
  if isinstance(v, bytes):
    return base64.b64encode(v).decode()
  return json.dumps(v, default=str) if isinstance(v, (list, dict)) else str(v)


def bt_type(t):
  if pa.types.is_integer(t):
    return "INT64"
  if pa.types.is_floating(t) or pa.types.is_decimal(t):
    return "FLOAT64"
  return "BOOL" if pa.types.is_boolean(t) else "STRING"


def page(tbl, total=0):
  """A result page as the UI wants it."""
  rows = zip(*(c.to_pylist() for c in tbl.columns))
  return dict(
      schema=[dict(name=f.name, type=bt_type(f.type)) for f in tbl.schema],
      rows=[dict(values=[wire(v) for v in r]) for r in rows],
      totalFilteredRows=total)


def type_for(cells):
  """The narrowest column type holding the cell types `cells`."""
  return next(t for t, holds in HOLDS.items() if not cells & ~holds)


def unique(names, taken):
  """Makes `names` unique, ignoring case as DuckDB does."""
  taken, out = {t.lower() for t in taken}, []
  for name in names:
    out.append(
        next(
            n for n in itertools.chain([name], (
                f"{name}_{i}" for i in itertools.count(2)))
            if n.lower() not in taken))
    taken.add(out[-1].lower())
  return out


def sanitize(name):
  name = re.sub(r"\W", "_", name.strip()) or "t"
  return f"t_{name}" if name[0].isdigit() else name


def where(filters, exprs):
  """A SQL condition and its parameters for DataGrid filters."""
  conds, params = ["true"], []
  for f in filters or []:
    col, op = exprs[f["field"]], f["op"].lower()
    if op in ("is null", "is not null"):
      conds.append(f"{col} {op}")
      continue
    if op in ("=", "!=", "<", "<=", ">", ">="):
      conds.append(f"{col} {op} ?")
    elif op in ("glob", "not glob", "in", "not in"):
      rhs = "GLOB ?" if "glob" in op else "IN (SELECT unnest(?::VARCHAR[]))"
      conds.append(f"{'NOT ' * op.startswith('not')}{col}::VARCHAR {rhs}")
    else:
      raise ValueError(f"Unsupported filter op: {op}")
    params.append(f["value"])
  return " AND ".join(conds), params


def order(order_by, exprs):
  """ORDER BY terms for an AIP-132 order_by such as "a desc, b"."""
  terms = [t.split() for t in (order_by or "").split(",") if t.strip()]
  return ", ".join(exprs[t[0]] + " DESC" * (t[1:] == ["desc"]) for t in terms)


def read_manifest(path):
  """The trace records and top-level keys of a JSON, CSV or SQLite manifest."""
  if path.endswith(".csv"):
    return duckdb.connect().read_csv(path).to_arrow_table().to_pylist(), {}
  if path.endswith(".db"):
    with contextlib.closing(sqlite3.connect(path)) as conn:
      conn.row_factory = sqlite3.Row
      return [dict(r) for r in conn.execute("SELECT * FROM traces")], {}
  with open(path, encoding="utf-8") as f:
    data = json.load(f)
  return (data, {}) if isinstance(data, list) else (data["traces"], data)


def list_traces(trace_dir, recursive):
  for dir_path, dirs, files in os.walk(trace_dir):
    dirs[:] = sorted(d for d in dirs if recursive and d[0] != ".")
    for f in sorted(files):
      if not NOT_TRACE.search(f):
        yield {"file_path": os.path.join(dir_path, f)}


class Bigtrace:
  """The trace catalog, the query executions and their DuckDB database.

  The methods taking a request `body` serve the ROUTES below.
  """

  def __init__(self, trace_dir, recursive, manifest, shell, workers, db_path,
               base_url, ui_origin):
    self.lock = threading.RLock()
    self.root = os.path.realpath(trace_dir)
    self.startup_manifest = manifest and os.path.realpath(manifest)
    self.shell, self.workers = shell, workers
    self.base_url, self.ui_origin = base_url, ui_origin
    self.db = duckdb.connect(
        db_path or ":memory:", config={"allow_unsigned_extensions": "true"})
    self.lib = load_extension(self.db)
    limit, unit = self.sql(
        "SELECT current_setting('memory_limit')").fetchone()[0].split()
    # Leaves the trace processors half of what DuckDB would take.
    self.db.execute(f"SET memory_limit = '{float(limit) / 2}{unit}'")
    self.db.execute("SET preserve_insertion_order = false")
    self.db.execute("SET enable_progress_bar = true")
    self.db.execute("SET enable_progress_bar_print = false")
    self.db.execute("CREATE TABLE IF NOT EXISTS _bigtrace_history "
                    "(query_uuid VARCHAR PRIMARY KEY, execution JSON)")
    # User SQL must not reach the file system, nor undo this.
    self.db.execute("SET enable_external_access = false")
    self.db.execute("SET lock_configuration = true")
    self.executions = {}
    tables = self.table_names()
    for query_uuid, ex in self.sql("FROM _bigtrace_history").fetchall():
      ex = self.executions[query_uuid] = json.loads(ex)
      if (ex["tableName"] or "").lower() in tables:
        ex["_columns"] = self.db.sql(f"FROM {q(ex['tableName'])}").columns
    self.load_catalog(trace_dir, recursive, manifest)

  def sql(self, query, params=None):
    """Runs `query` on a fresh cursor: threads must not share one."""
    with self.lock:
      cur = self.db.cursor()
    return cur.execute(query, params)

  def close(self):
    for query_uuid in list(self.executions):
      self.cancel(None, query_uuid)

  # Traces --------------------------------------------------------------------

  def load_catalog(self, trace_dir, recursive, manifest=None):
    """Rebuilds `trace_metadata` from a manifest, or a directory listing."""
    for path in (trace_dir, manifest):
      real = path and os.path.realpath(path)
      if real and real != self.startup_manifest and os.path.commonpath(
          [real, self.root]) != self.root:
        raise ValueError(f"{path} is outside the served directory {self.root}")
    found = [
        f"{trace_dir}/{m}" for m in ("bigtrace_manifest.json", "metadata.db")
    ]
    manifest = manifest or next(filter(os.path.exists, found), None)
    records, extra = ({}, {}) if not manifest else read_manifest(manifest)
    self.trace_dir, self.recursive = os.path.abspath(trace_dir), recursive
    self.manifest = manifest and os.path.abspath(manifest)
    self.columns_info = {c["name"]: c for c in extra.get("schema", [])}
    self.presets = extra.get("presets", [])
    self.experiments = [camel(e) for e in extra.get("experiments", [])]
    if not manifest:
      return self.set_traces(list_traces(trace_dir, recursive), trace_dir)
    base = os.path.join(os.path.dirname(manifest), extra.get("trace_dir", "."))
    return self.set_traces(records, base)

  def set_traces(self, records, base_dir):
    """Makes `trace_metadata` hold the traces of manifest `records`."""
    rows = []
    for r in records:
      path = r.pop("absolute_local_file_path", None) or r["file_path"]
      path = os.path.normpath(os.path.join(base_dir, path))
      st = os.stat(path) if os.path.exists(path) else None
      r = {k: v for k, v in r.items() if v is not None}
      trace_uuid = str(
          r.get("trace_uuid") or
          uuid.UUID(bytes=hashlib.md5(path.encode()).digest()))
      name = r.get("file_name") or os.path.basename(path)
      url = urllib.parse.quote(
          f"{self.base_url}/trace/{trace_uuid}/{name}", safe=":/")
      mtime = st and datetime.datetime.fromtimestamp(st.st_mtime,
                                                     datetime.timezone.utc)
      defaults = dict(
          _trace_uuid=trace_uuid,
          link=f"{self.ui_origin}/#!/?url={url}"
          "&referrer=bigtrace_server",
          file_name=name,
          file_size=st and st.st_size,
          modified_time=mtime and mtime.isoformat())
      rows.append({
          **defaults,
          **r, "trace_uuid": trace_uuid,
          "file_path": path
      })
    keys = dict.fromkeys(k for r in rows for k in r) or ["_trace_uuid"]
    table = pa.table({k: [r.get(k) for r in rows] for k in keys})
    with self.lock:
      cur = self.db.cursor()
    cur.register("traces", table)
    cur.execute("CREATE OR REPLACE TABLE trace_metadata AS FROM traces")
    return {"traceCount": len(rows)}

  def apply_settings(self, settings):
    """Switches to the trace source the UI settings name; returns them."""
    s = {x["setting_id"]: x["values"] for x in settings or [] if x["values"]}
    manifest = s.get("trace_manifest", [self.manifest])[0]
    source = (os.path.abspath(s.get("trace_directory", [self.trace_dir])[0]),
              str(s.get("recursive",
                        [self.recursive])[0]).lower() == "true", manifest and
              os.path.abspath(manifest))
    if source != (self.trace_dir, self.recursive, self.manifest):
      self.load_catalog(*source)
    return s

  def metadata_schema(self):
    fields = self.sql("FROM trace_metadata LIMIT 0").arrow().schema
    return [{
        "name": f.name,
        "displayName": f.name.replace("_", " ").title(),
        "type": bt_type(f.type),
        "defaultVisible": f.name not in ("file_path", "modified_time"),
        "description": "",
        **self.columns_info.get(f.name, {}),
    } for f in fields if f.name != "_trace_uuid"]

  def page(self,
           source,
           exprs,
           body,
           columns,
           cond="true",
           params=(),
           total=None):
    """A page of `source` for a DataGrid request `body`, and its row count."""
    filtered, more_params = where(body.get("filters"), exprs)
    params = [*params, *more_params]
    query = f"FROM {source} WHERE {cond} AND {filtered}"
    if total is None or body.get("filters"):
      total = self.sql(f"SELECT count(*) {query}", params).fetchone()[0]
    sort = order(body.get("order_by"), exprs)
    query += f" ORDER BY {sort}" * bool(sort)
    if body.get("limit") is not None:
      query += f" LIMIT {int(body['limit'])} OFFSET {int(body.get('offset', 0))}"
    select = ", ".join(f"{exprs[c]} AS {q(c)}" for c in columns if c in exprs)
    return self.sql(f"SELECT {select} {query}", params).to_arrow_table(), total

  def traces(self, body, columns=None):
    """The traces a /trace_metadata request selects, and how many match."""
    settings = self.apply_settings(body.get("settings"))
    cond, params = "true", []
    if settings.get("trace_uuids"):
      cond = "list_has_any(?, [trace_uuid, file_name])"
      params = [settings["trace_uuids"]]
    if body.get("experiment_filter"):
      e = body["experiment_filter"]
      cond += (" AND experiment_id = ? AND control_id = ? AND "
               "coalesce(is_treatment::BOOLEAN = ?, true)")
      params += [e["experiment_id"], e["control_id"], e["is_treatment"]]
    schema = self.metadata_schema()
    columns = columns or body.get("columns") or [
        c["name"] for c in schema if c["defaultVisible"]
    ]
    exprs = {c: q(c) for c in ["_trace_uuid", *(c["name"] for c in schema)]}
    return self.page("trace_metadata", exprs, body, columns, cond, params)

  def trace_list(self, body):
    tbl, total = self.traces({"limit": 50, **body})
    rows = page(tbl)["rows"]
    return dict(
        columnNames=tbl.column_names, rows=rows, totalFilteredRows=total)

  def trace_metadata_schema(self, body):
    self.apply_settings(body.get("settings"))
    return {"columns": self.metadata_schema()}

  def info(self, _):
    count = self.sql("SELECT count(*) FROM trace_metadata").fetchone()[0]
    return dict(
        status="ok",
        service="bigtrace",
        features=["duckdb"],
        traceCount=count,
        traceDir=self.trace_dir,
        manifest=self.manifest)

  def refresh(self, _):
    return self.load_catalog(self.trace_dir, self.recursive, self.manifest)

  def execution_config(self, _):
    """The trace source settings the UI shows."""
    settings = [("trace_directory", "plainString", self.trace_dir),
                ("recursive", "booleanOptions", self.recursive),
                ("trace_manifest", "plainString", self.manifest or ""),
                ("trace_uuids", "stringArray", [])]
    return {
        "setting": [{
            "id": name,
            "name": name.replace("_", " ").capitalize(),
            "category": "TRACE_ADDRESS",
            "disabled": name == "trace_uuids",
            kind: {
                "defaultValues" if kind == "stringArray" else "defaultValue": v
            },
        } for name, kind, v in settings]
    }

  def trace_presets(self, _):
    return {"tracePresets": self.presets}

  def experiments_search(self, body):
    term, offset = body.get("search_query", "").lower(), body.get("offset", 0)
    found = [
        e for e in self.experiments
        if term in e.get("experimentName", "").lower() or
        term in e.get("controlName", "").lower()
    ]
    return {"experiments": found[offset:offset + body.get("limit", 50)]}

  def experiment(self, body):
    return next(({
        "experiment": e
    }
                 for e in self.experiments
                 if e.get("experimentId") == body["experiment_id"]), {})

  def trace_file(self, _, trace_uuid):
    row = self.sql("SELECT file_path FROM trace_metadata WHERE _trace_uuid = ?",
                   [trace_uuid]).fetchone()
    if not row or not os.path.isfile(row[0]):
      raise HttpError(404, f"No trace {trace_uuid}")
    return open(row[0], "rb")

  # Tables --------------------------------------------------------------------

  def table_names(self, running=False):
    """Lower-case table and view names, plus those running queries write."""
    names = {
        e["tableName"].lower()
        for e in self.executions.values()
        if running and e["status"] == "IN_PROGRESS" and e["tableName"]
    }
    return names | {
        r[0].lower()
        for r in self.sql("SELECT table_name FROM duckdb_tables() UNION "
                          "SELECT view_name FROM duckdb_views()").fetchall()
    }

  def check_table(self, body):
    name = sanitize(body["table_name"])
    taken = self.table_names(running=True)
    suggested = next(f"{name}_{i}" for i in itertools.count(1)
                     if f"{name}_{i}".lower() not in taken)
    return dict(
        exists=name.lower() in taken,
        resolvedTableName=name,
        suggestedTableName=suggested)

  def new_table_name(self, requested, taken):
    """`requested`, unless reserved or running queries write it, or result_N."""
    if requested:
      busy = {
          e["tableName"]
          for e in self.executions.values()
          if e["status"] == "IN_PROGRESS"
      } | taken
      busy |= {"latest", "trace_metadata"}
      return unique([sanitize(requested).lstrip("_") or "t"], busy)[0]
    used = [
        re.fullmatch(r"result_(\d+)", t)
        for t in self.table_names(running=True) | taken
    ]
    return f"result_{max([int(m[1]) for m in used if m], default=0) + 1}"

  def tables(self, _):
    return {
        "tables":
            self.sql("""
        SELECT table_name AS name, estimated_size AS rowCount,
          list({'name': column_name, 'type': data_type}) AS columns
        FROM duckdb_tables() JOIN duckdb_columns() USING (table_oid, table_name)
        WHERE NOT starts_with(table_name, '_bigtrace')
        GROUP BY ALL ORDER BY name""").to_arrow_table().to_pylist()
    }

  # Queries -------------------------------------------------------------------

  def get(self, query_uuid):
    if query_uuid not in self.executions:
      raise HttpError(404, f"Query {query_uuid} not found")
    return self.executions[query_uuid]

  def execution(self, _, query_uuid):
    ex = self.get(query_uuid)
    return {k: v for k, v in ex.items() if not k.startswith("_")}

  def list_executions(self, _):
    exs = [self.execution(None, u) for u in self.executions]
    return {"queryExecutions": sorted(exs, key=lambda e: e["startTime"])[::-1]}

  def status(self, _, query_uuid):
    ex = self.get(query_uuid)
    out = {k: ex[k] for k in STATUS if ex.get(k) is not None}
    if ex.get("_cursor"):  # DuckDB's progress, as percent of 100 "traces".
      out["processedTraces"] = max(0, int(ex["_cursor"].query_progress()))
      out["totalTraces"] = 100
    return out

  def execute_async(self, body):
    return self.execute(body, sync=False)

  def execute(self, body, sync=True):
    """Starts the queries of `body`. A sync call returns the first's rows."""
    engine = body.get("engine", "traces")
    materialized = body.get("materialized", True)
    limit, traces, exs = int(body.get("limit", 10000)), [], []
    if engine == "traces":
      request = dict(
          settings=body.get("settings"),
          filters=body.get("trace_filters"),
          order_by=body.get("trace_order_by"),
          limit=body.get("trace_limit") or None,
          experiment_filter=body.get("experiment_filter"))
      columns = ["_trace_uuid", "file_path", "file_name", "file_size"]
      tbl = self.traces(request, columns)[0]
      traces = list(zip(*tbl.to_pydict().values()))
    meta = list(body.get("trace_metadata_columns") or [])
    meta += ["_trace_uuid"] * (engine == "traces" and "_trace_uuid" not in meta)
    with self.lock:
      for spec in body.get("queries") or [body]:
        table = None
        if engine == "traces" or materialized:
          table = self.new_table_name(
              spec.get("table_name"), {e["tableName"] for e in exs})
        ex = dict(
            queryUuid=str(uuid.uuid4()),
            status="IN_PROGRESS",
            startTime=now(),
            processedRows=0,
            processedTraces=0,
            totalTraces=len(traces) if engine == "traces" else 1,
            perfettoSql=spec["perfetto_sql"],
            queryEngine=engine,
            limit=limit,
            traceLimit=body.get("trace_limit"),
            materialized=materialized,
            tableName=table,
            schema=[],
            settings=[camel(s) for s in body.get("settings") or []],
            traceFilters=body.get("trace_filters"),
            traceMetadataColumns=meta,
            traceOrderBy=body.get("trace_order_by"),
            experimentFilter=camel(body.get("experiment_filter") or {}),
            _errors=[],
            _partial=[],
            _ok=0)
        self.executions[ex["queryUuid"]] = ex
        exs.append(ex)
    if engine == "traces":
      run = lambda: self.run_traces(exs, traces)
    else:
      run = lambda: [self.run_duckdb(ex) for ex in exs]
    head = dict(
        queryUuid=exs[0]["queryUuid"],
        tableName=exs[0]["tableName"],
        executions=[{
            k: e[k] for k in STATUS[:5:4]
        } for e in exs])
    if not sync:
      threading.Thread(target=run, daemon=True).start()
      return head
    run()
    ex = next((e for e in exs if e["status"] == "SUCCESS"), exs[0])
    if ex["status"] == "FAILED":
      raise ValueError(ex["errorMessage"])
    if "_rows" in ex:
      return (head, *ex.pop("_rows"))
    return (head, *self.fetch(ex["queryUuid"], {"limit": limit or None})[1:])

  def run_duckdb(self, ex):
    with self.lock:
      ex["_cursor"] = cur = self.db.cursor()
    try:
      sql = cur.execute("SELECT pf_compile_pipe(?)",
                        [ex["perfettoSql"]]).fetchone()[0]
      rel = cur.sql(sql)  # None for statements without rows.
      limit = ex["limit"]
      if rel is not None and ex["tableName"]:
        (rel.limit(limit) if limit else rel).create_view("_bigtrace_query")
        cur.execute(f"CREATE OR REPLACE TABLE {q(ex['tableName'])} AS "
                    "FROM _bigtrace_query")
        ex["_columns"] = cur.sql(f"FROM {q(ex['tableName'])} LIMIT 0").columns
      elif rel is not None:
        tbl = (rel.limit(limit + 1) if limit else rel).to_arrow_table()
        ex["_rows"] = tbl.slice(0, limit or None), tbl.num_rows
      else:
        ex["tableName"] = None
      self.finish(ex, "SUCCESS")
    except duckdb.Error as e:
      ex["errorMessage"] = str(e)
      self.finish(ex, "FAILED")

  def run_traces(self, exs, traces):
    uuids = [t[0] for t in traces]
    for ex in exs:
      ex["_handle"] = self.lib.pf_open(ex["perfettoSql"].encode(),
                                       ctypes.c_int64(ex["limit"]))
      ex["_drain"] = threading.Thread(target=self.drain, args=(ex, uuids))
      ex["_drain"].start()
    todo = list(range(len(traces)))
    if not any(ex["limit"] for ex in exs):
      # Big traces load longest: started last, they would leave a long tail.
      todo.sort(key=lambda i: -(traces[i][3] or 0))
    with ThreadPoolExecutor(self.workers) as pool:
      list(pool.map(lambda i: self.query_trace(exs, i, traces[i]), todo))
    for ex in exs:
      self.lib.pf_close(ex["_handle"])
      ex["_drain"].join()
      self.lib.pf_free(ex["_handle"])
      if ex["_partial"] and ex.get("_columns"):
        self.sql(
            f"DELETE FROM {q(ex['tableName'])} WHERE list_contains(?, _trace_uuid::VARCHAR)",
            [ex["_partial"]])
      if ex["_errors"]:
        ex["errorMessage"] = (f"{len(ex['_errors'])} trace(s) failed: " +
                              "; ".join(ex["_errors"][:3]))
      failed = ex.get("_fatal") or (ex["_errors"] and not ex["_ok"])
      self.finish(ex, "FAILED" if failed else "SUCCESS")

  def query_trace(self, exs, ord, trace):
    """Loads one trace in a shell and runs the queries still running on it."""
    live = [
        e for e in exs if e["status"] == "IN_PROGRESS" and not e.get("_full")
    ]
    if not live:
      return
    # setpriv: the shell gets SIGKILL when this thread, or the server, dies.
    cmd = [
        "setpriv", "--pdeathsig", "KILL", self.shell, "--stdiod",
        "--no-ftrace-raw", trace[1]
    ]
    with tempfile.TemporaryFile() as stderr, subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=stderr) as shell:
      for i, ex in enumerate(live):
        rows, msg = ctypes.c_int64(), ctypes.create_string_buffer(4096)
        status = self.lib.pf_feed(ex["_handle"], shell.stdin.fileno(),
                                  shell.stdout.fileno(), ord, i + 1 < len(live),
                                  ctypes.byref(rows), msg, len(msg))
        error = msg.value.decode(errors="replace")
        if status == FEED_EXITED:  # Its last words say why.
          shell.wait()
          stderr.seek(0)
          error = (stderr.read().decode(errors="replace").strip() or
                   error).splitlines()[-1]
        with self.lock:
          ex["processedTraces"] += 1
          if status in (FEED_OK, FEED_LIMIT):
            ex["_ok"] += 1
            ex["processedRows"] += rows.value
            ex["_full"] = status == FEED_LIMIT
          elif status != FEED_CANCELLED:
            ex["_errors"].append(f"{trace[2]}: {error}")
            ex["_partial"] += [trace[0]] * bool(rows.value)
      shell.kill()

  def drain(self, ex, uuids):
    """Creates the result table once column types are known, then fills it."""
    h, max_cols = ex["_handle"], 1 << 14
    names = ctypes.create_string_buffer(1 << 22)
    cells = (ctypes.c_uint32 * max_cols)()
    n = self.lib.pf_wait_typed(h, names, len(names), cells, max_cols)
    if n < 0:
      ex["tableName"] = None  # No trace returned any columns.
      return
    cols = [c.decode(errors="replace") for c in names.raw.split(b"\0")[:n]]
    cols, types = unique(cols, ["_trace_uuid"]), list(map(type_for, cells[:n]))
    table = q(ex["tableName"])
    enum = ", ".join("'" + u.replace("'", "''") + "'" for u in uuids)
    try:
      self.sql(f"CREATE OR REPLACE TABLE {table} (" +
               "".join(f"{q(c)} {t}, " for c, t in zip(cols, types)) +
               f"_trace_uuid ENUM({enum}))")
      ex["_columns"] = cols + ["_trace_uuid"]
      while True:
        ex["schema"] = page(self.fetch(ex["queryUuid"],
                                       {"limit": 0})[1])["schema"]
        # Ends after 5 s, to commit what is there, when a column must widen
        # (to hold a double, say) and when all traces are drained.
        self.sql(f"INSERT INTO {table} FROM pf_scan(?, ?, ?, ?, 5000)",
                 [h, max(1,
                         os.cpu_count() // 2), types, uuids])
        if self.lib.pf_take_stop(h) == 0:
          break
        self.lib.pf_wait_typed(h, names, len(names), cells, max_cols)
        for i, (col, old) in enumerate(zip(cols, types)):
          types[i] = type_for(cells[i] | HOLDS[old])
          if types[i] != old:
            self.sql(f"ALTER TABLE {table} ALTER {q(col)} TYPE {types[i]}")
    except duckdb.Error as e:
      ex["_errors"].append(f"DuckDB: {e}")
      ex["_fatal"] = True
      self.lib.pf_cancel(h)

  def finish(self, ex, status):
    with self.lock:
      ex.pop("_cursor", None)
      if ex["queryUuid"] not in self.executions:  # Deleted while running.
        return self.drop_table(ex["tableName"])
      final_status = status if ex["status"] == "IN_PROGRESS" else ex["status"]
    if ex.get("_columns"):
      table = q(ex["tableName"])
      ex["processedRows"] = self.sql(
          f"SELECT count(*) FROM {table}").fetchone()[0]
      ex["schema"] = page(self.fetch(ex["queryUuid"],
                                     {"limit": 0})[1])["schema"]
      self.sql(f"CREATE OR REPLACE VIEW latest AS FROM {table}")
    end_time = now()
    record = {
        **self.execution(None, ex["queryUuid"]),
        "status": final_status,
        "endTime": end_time,
    }
    self.sql("INSERT OR REPLACE INTO _bigtrace_history VALUES (?, ?)",
             [ex["queryUuid"], json.dumps(record)])
    with self.lock:
      if ex["status"] == "IN_PROGRESS":
        ex["status"] = status
      ex["endTime"] = end_time

  def drop_table(self, table):
    """Drops `table`, unless a remaining query wrote it last."""
    if table and table not in {
        e["tableName"] for e in self.executions.values()
    }:
      self.sql(f"DROP TABLE IF EXISTS {q(table)}")

  def cancel(self, _, query_uuid):
    ex = self.get(query_uuid)
    with self.lock:
      if ex["status"] == "IN_PROGRESS":
        ex["status"] = "CANCELLED"
        if "_handle" in ex:
          self.lib.pf_cancel(ex["_handle"])
        if ex.get("_cursor"):
          ex["_cursor"].interrupt()
    return {}

  def delete(self, _, query_uuid):
    self.cancel(None, query_uuid)
    with self.lock:
      ex = self.executions.pop(query_uuid)
    self.drop_table(ex["tableName"])
    self.sql("DELETE FROM _bigtrace_history WHERE query_uuid = ?", [query_uuid])
    return {}

  def fetch_results(self, body, query_uuid):
    return self.fetch(query_uuid, body)

  def fetch(self, query_uuid, body):
    """A page of a query's table: (response head, Arrow table, row count)."""
    ex = self.get(query_uuid)
    cols, traces = ex.get("_columns") or [], ex["queryEngine"] == "traces"
    meta = [f["name"] for f in self.metadata_schema()] if traces else []
    exprs = {c: f"m.{q(c)}" for c in meta} | {c: f"t.{q(c)}" for c in cols}
    head = dict(
        queryUuid=query_uuid,
        tableName=ex["tableName"],
        availableColumnNames=list(exprs))
    if not cols:
      return head, pa.table({}), 0
    source = f"{q(ex['tableName'])} t"
    source += " LEFT JOIN trace_metadata m USING (_trace_uuid)" * traces
    default = cols[:-1] + ex["traceMetadataColumns"] if traces else cols
    return (head, *self.page(
        source,
        exprs,
        body,
        body.get("columns") or default,
        total=ex.get("processedRows")))


ROUTES = [(m, re.compile(p), name) for m, p, name in [
    ("GET", r"/", "info"),
    ("POST", r"/refresh", "refresh"),
    ("GET", r"/duckdb/tables", "tables"),
    ("GET", r"/trace_presets", "trace_presets"),
    ("GET", r"/trace/([^/]+)/[^/]+", "trace_file"),
    ("POST", r"/bigtrace_execution_config", "execution_config"),
    ("POST", r"/trace_metadata_schema", "trace_metadata_schema"),
    ("POST", r"/trace_metadata", "trace_list"),
    ("POST", r"/experiments", "experiments_search"),
    ("POST", r"/experiment_metadata", "experiment"),
    ("POST", r"/execute_bigtrace_query", "execute"),
    ("POST", r"/execute_bigtrace_query_async", "execute_async"),
    ("POST", r"/check_table_exists", "check_table"),
    ("GET", r"/query_executions", "list_executions"),
    ("GET", r"/query_executions/([^/:]+)", "execution"),
    ("DELETE", r"/query_executions/([^/:]+)", "delete"),
    ("GET", r"/query_executions/([^/:]+):status", "status"),
    ("POST", r"/query_executions/([^/:]+):fetch_results", "fetch_results"),
    ("POST", r"/query_executions/([^/:]+):cancel", "cancel"),
]]


class Handler(http.server.BaseHTTPRequestHandler):
  """Serves ROUTES, refusing requests made by other websites."""
  bigtrace = None
  origins = ("https://ui.perfetto.dev",)  # Allowed, besides localhost ones.
  hosts = ("localhost", "127.0.0.1", "::1")  # Allowed in the Host header.

  def log_message(self, *args):
    pass

  def end_headers(self):
    self.send_header("Access-Control-Allow-Origin",
                     self.headers.get("Origin", "*"))
    self.send_header("Access-Control-Allow-Credentials", "true")
    self.send_header("Access-Control-Allow-Private-Network", "true")
    self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE")
    self.send_header("Access-Control-Allow-Headers", "Content-Type")
    self.send_header("Vary", "Origin")
    super().end_headers()

  def send(self, data, ctype="application/json", status=200, headers=()):
    """Sends JSON, bytes or a file."""
    data = json.dumps(data).encode() if ctype == "application/json" else data
    file = hasattr(data, "read")
    size = os.fstat(data.fileno()).st_size if file else len(data)
    self.send_response(status)
    for header, value in [("Content-Type", ctype), ("Content-Length", size),
                          *headers]:
      self.send_header(header, str(value))
    self.end_headers()
    shutil.copyfileobj(data, self.wfile) if file else self.wfile.write(data)

  def do_GET(self):
    self.dispatch("GET")

  def do_POST(self):
    self.dispatch("POST")

  def do_DELETE(self):
    self.dispatch("DELETE")

  def do_OPTIONS(self):
    self.dispatch("OPTIONS")

  def dispatch(self, method):
    path = re.sub("/+", "/", urllib.parse.urlsplit(self.path).path)
    path = re.sub(r"^/v1(?=/|$)", "", path).rstrip("/") or "/"
    origin = self.headers.get("Origin")
    host = urllib.parse.urlsplit("//" + self.headers.get("Host", "")).hostname
    try:
      if host not in self.hosts:
        raise HttpError(403, "Invalid Host header")
      if origin and not ({"*", origin} & set(self.origins) or
                         urllib.parse.urlsplit(origin).hostname
                         in Handler.hosts):
        raise HttpError(403, "Origin not allowed")
      if method == "OPTIONS":
        return self.send(b"", "text/plain")
      size = int(self.headers.get("Content-Length") or 0)
      if size and self.headers.get_content_type() != "application/json":
        raise HttpError(415, "Content-Type must be application/json")
      body = json.loads(self.rfile.read(size)) if size else {}
      routes = [(m, n, mt) for m, p, n in ROUTES if (mt := p.fullmatch(path))]
      route = next(((n, mt) for m, n, mt in routes if m == method), None)
      if not route:
        raise HttpError(405 if routes else 404, f"Cannot {method} {path}")
      args = map(urllib.parse.unquote, route[1].groups())
      res = getattr(self.bigtrace, route[0])(body, *args)
      if isinstance(res, tuple) and body.get("format") == "csv":
        out = pa.BufferOutputStream()
        pyarrow.csv.write_csv(res[1], out)
        more = [("X-Bigtrace-Truncated", "1")] * (res[2] > res[1].num_rows)
        self.send(out.getvalue().to_pybytes(), "text/csv", headers=more)
      elif isinstance(res, tuple):
        self.send({**res[0], **page(res[1], res[2])})
      elif hasattr(res, "read"):
        with res:
          self.send(res, "application/octet-stream")
      else:
        self.send(res)
    except HttpError as e:
      self.send({"detail": str(e)}, status=e.status)
    except (KeyError, TypeError, ValueError, duckdb.Error) as e:
      self.send({"detail": str(e)}, status=400)
    except (BrokenPipeError, ConnectionResetError):
      pass  # The client went away.
    except Exception as e:  # pylint: disable=broad-except
      logging.exception("%s %s failed", method, path)
      self.send({"detail": str(e)}, status=500)
