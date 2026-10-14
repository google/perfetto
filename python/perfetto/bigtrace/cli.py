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
"""Command line of the local Bigtrace server: serve traces and query them."""

import argparse
import hashlib
import http.server
import itertools
import json
import os
import re
import shutil
import signal
import socket
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

from perfetto.bigtrace.server import (CACHE_DIR, Bigtrace, Handler, list_traces,
                                      q, read_manifest)

SERVER = "http://127.0.0.1:5001"
# Per-trace metadata that --generate-manifest extracts.
METADATA_SQL = "SELECT " + ", ".join([
    "(SELECT start_ts FROM trace_bounds) AS start_ts",
    "(SELECT end_ts FROM trace_bounds) AS end_ts",
    "(SELECT (end_ts - start_ts) / 1e6 FROM trace_bounds) AS duration_ms",
    "(SELECT count(1) FROM process) AS process_count",
    "(SELECT count(1) FROM thread) AS thread_count",
    "(SELECT count(1) FROM slice) AS slice_count",
] + [
    f"extract_metadata('{k}') AS {k}"
    for k in ("trace_uuid", "android_build_fingerprint", "android_sdk_version",
              "system_machine", "system_name", "system_release", "trace_type")
])

HELP = """\
Bigtrace runs PerfettoSQL on many traces at once, one trace_processor_shell
per trace, and collects the rows in DuckDB tables (with a _trace_uuid column)
that you can then analyse with DuckDB SQL, here or in the Bigtrace UI.

examples:
  %(prog)s serve ~/traces -r                      # serve a directory of traces
  %(prog)s query "SELECT name, dur FROM slice" --table slices
  %(prog)s query --filter "duration_ms > 1000" "SELECT count(*) AS n FROM slice"
  %(prog)s sql "SELECT name, sum(dur) FROM slices GROUP BY 1 ORDER BY 2 DESC"
  %(prog)s sql "FROM duckdb_tables()"             # list the result tables
  %(prog)s sql "SELECT execution->>'tableName', execution->>'status'
               FROM _bigtrace_history"            # query history
"""

QUERY_HELP = """\
Runs PerfettoSQL on every selected trace of the server, in one pass (each
trace is loaded once for all queries), into one DuckDB table per query. Prints
the first --limit rows of each table as CSV on stdout, and the table name, row
count and per-trace errors on stderr. Ctrl-C cancels the queries.

exit status: 0 on success, 1 if a query failed (or the request did), 2 if
some traces failed but the queries succeeded on the others.
"""

SQL_HELP = """\
Runs DuckDB SQL on the server's database, which holds the result tables of
`query`, the trace_metadata table (one row per trace, joinable on
_trace_uuid), `latest` (the last result table) and _bigtrace_history. Prints
the first --limit rows as CSV on stdout. Statements (CREATE TABLE ... AS,
DROP TABLE, ...) work too.
"""


def main(argv=None):
  argv = sys.argv[1:] if argv is None else argv
  if argv and argv[0] not in ("serve", "query", "sql", "-h", "--help"):
    argv = ["serve", *argv]  # Plain `bigtrace_server.py TRACES` serves.
  fmt = argparse.RawDescriptionHelpFormatter
  parser = argparse.ArgumentParser(description=HELP, formatter_class=fmt)
  cmds = parser.add_subparsers(dest="command", required=True)

  serve = cmds.add_parser(
      "serve",
      help="serve traces to the UI and the CLI",
      description=serve_main.__doc__,
      formatter_class=fmt)
  serve.add_argument(
      "traces",
      help="directory of traces, or an archive (zip, "
      "tar...) of them, which is extracted (once) and served -r")
  serve.add_argument(
      "-r",
      "--recursive",
      action="store_true",
      help="also serve the traces of subdirectories")
  serve.add_argument(
      "-m",
      "--manifest",
      help="JSON, CSV (.csv) or SQLite "
      "(.db) manifest of the traces and their metadata "
      "(default: bigtrace_manifest.json or metadata.db in TRACES, "
      "if there)")
  serve.add_argument(
      "-p",
      "--port",
      type=int,
      default=5001,
      help="port, 0 for any free one (default: %(default)s)")
  serve.add_argument(
      "--host",
      default="127.0.0.1",
      help="address to listen "
      "on (default: %(default)s); anyone who can reach it can "
      "read the traces")
  serve.add_argument(
      "-w",
      "--workers",
      type=int,
      help="traces loaded at a "
      "time (default: CPUs, at most one per 2 GiB of RAM)")
  serve.add_argument(
      "--shell-path",
      default=os.environ.get("SHELL_PATH"),
      help="trace_processor_shell (default: $SHELL_PATH, else "
      "a prebuilt one is downloaded)")
  serve.add_argument(
      "--duckdb-path",
      help="DuckDB file to keep result tables "
      "and history in across restarts (default: in memory)")
  serve.add_argument(
      "--origin",
      default="https://ui.perfetto.dev",
      help="Perfetto UI that trace links open (default: "
      "%(default)s)")
  serve.add_argument(
      "--cors-origin",
      action="append",
      default=[],
      help="another website allowed to call the server; "
      "repeatable")
  serve.add_argument(
      "--generate-manifest",
      nargs="?",
      metavar="OUT",
      const="bigtrace_manifest.json",
      help="extract the "
      "metadata of every trace into a manifest (default OUT: "
      "bigtrace_manifest.json in TRACES) before serving; "
      "unchanged traces are reused from OUT")
  serve.add_argument(
      "-f",
      "--force",
      action="store_true",
      help="with --generate-manifest: extract every trace again")
  serve.add_argument(
      "--metadata-query",
      help="with --generate-manifest: SQL "
      "whose first row adds columns to each trace")
  serve.set_defaults(main=serve_main)

  query = cmds.add_parser(
      "query",
      help="run PerfettoSQL on many traces",
      description=QUERY_HELP,
      formatter_class=fmt)
  query.add_argument("sql", nargs="+", help="PerfettoSQL query, one per table")
  query.add_argument(
      "--table",
      action="append",
      default=[],
      help="name of "
      "the result table of each query (default: result_N); an "
      "existing table is replaced")
  query.add_argument(
      "--filter",
      action="append",
      default=[],
      metavar="'COLUMN OP VALUE'",
      help="only the traces whose "
      "trace_metadata COLUMN matches (OP: = != < <= > >= glob); "
      "repeatable")
  query.add_argument("--trace-limit", type=int, help="at most this many traces")
  query.set_defaults(main=query_main)

  sql = cmds.add_parser(
      "sql",
      help="run DuckDB SQL on the result tables",
      description=SQL_HELP,
      formatter_class=fmt)
  sql.add_argument("sql", help="DuckDB SQL")
  sql.set_defaults(main=sql_main)
  for cmd in (query, sql):
    cmd.add_argument("--server", default=SERVER, help="default: %(default)s")
    cmd.add_argument(
        "--limit",
        type=int,
        default=100,
        help="rows to print "
        "(default: %(default)s); aggregate in SQL rather than "
        "printing whole tables")
  args = parser.parse_args(argv)
  return args.main(args)


def serve_main(args):
  """Serves traces to the Bigtrace UI and to the `query` and `sql` commands.

  Open the UI at https://ui.perfetto.dev/bigtrace.html?server=URL (or a local
  one, `ui/run-dev-server --bigtrace`).
  """
  traces, recursive = args.traces, args.recursive
  if os.path.isfile(traces):
    traces, recursive = unpack(traces), True
  origins = (*Handler.origins, args.origin.rstrip("/"), *args.cors_origin)
  handler = type("Handler", (Handler,), {
      "origins": origins,
      "hosts": (*Handler.hosts, args.host, socket.getfqdn()),
  })
  server_class = type("Server", (http.server.ThreadingHTTPServer,), {
      "address_family": socket.AF_INET6 if ":" in args.host else socket.AF_INET
  })
  httpd = server_class((args.host, args.port), handler)
  host = "127.0.0.1" if args.host == "0.0.0.0" else args.host
  url = f"http://{f'[{host}]' if ':' in host else host}:{httpd.server_port}"
  ram = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
  workers = args.workers or min(os.cpu_count(), ram >> 31)
  shell = args.shell_path
  if not shell:
    from perfetto.trace_processor.platform import PlatformDelegate
    shell = PlatformDelegate().get_shell_path(bin_path=None)
  handler.bigtrace = bigtrace = Bigtrace(traces, recursive, args.manifest,
                                         shell, workers, args.duckdb_path, url,
                                         args.origin.rstrip("/"))
  if args.generate_manifest:
    out = os.path.join(traces, args.generate_manifest)
    generate_manifest(bigtrace, out, args.force, args.metadata_query)
    bigtrace.load_catalog(traces, recursive, out)
  count = bigtrace.info(None)["traceCount"]
  print(
      f"Serving {count} traces from {bigtrace.manifest or bigtrace.trace_dir}"
      f" on {url} with {workers} workers.\nUI: {args.origin}/bigtrace.html"
      f"?server={url}",
      flush=True)
  signal.signal(signal.SIGTERM, signal.default_int_handler)
  try:
    httpd.serve_forever()
  except KeyboardInterrupt:
    pass
  finally:
    bigtrace.close()


def unpack(archive):
  """Extracts an archive of traces, once per version of it."""
  st = os.stat(archive)
  key = f"{os.path.abspath(archive)}:{st.st_size}:{st.st_mtime_ns}".encode()
  out = f"{CACHE_DIR}/{os.path.basename(archive)}-{hashlib.sha1(key).hexdigest()[:12]}"
  if not os.path.isdir(out):
    os.makedirs(CACHE_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory(
        dir=CACHE_DIR, ignore_cleanup_errors=True) as tmp:
      print(f"Extracting {archive}", flush=True)
      # zipfile refuses unsafe paths itself; tarfile needs the filter.
      safe = {} if zipfile.is_zipfile(archive) else {"filter": "data"}
      shutil.unpack_archive(archive, tmp, **safe)
      os.rename(tmp, out)
  return out


def generate_manifest(bigtrace, out, force, metadata_query):
  """Writes a manifest of the traces' metadata, extracted in parallel.

  Traces whose size and modification time did not change keep their entry.
  """
  old = {} if force or not os.path.exists(out) else read_manifest(out)[1]
  cached = {t["file_path"]: t for t in old.get("traces", [])}
  root = bigtrace.trace_dir
  bigtrace.set_traces(list_traces(root, bigtrace.recursive), root)
  traces, todo = [], {}
  for trace_uuid, path, size, mtime in bigtrace.sql(
      "SELECT _trace_uuid, file_path, file_size, modified_time "
      "FROM trace_metadata").fetchall():
    entry = dict(
        file_path=os.path.relpath(path, root),
        file_size=size,
        modified_time=mtime)
    old_entry = cached.get(entry["file_path"], {})
    if [old_entry.get("file_size"),
        old_entry.get("modified_time")] == [size, mtime]:
      traces.append(old_entry)
    else:
      todo[trace_uuid] = {**entry, "abs_path": path}
  print(f"Extracting the metadata of {len(todo)} traces", flush=True)
  if todo:
    paths = [t.pop("abs_path") for t in todo.values()]
    head = bigtrace.execute({
        "queries": [{
            "perfetto_sql": s
        } for s in (METADATA_SQL, metadata_query) if s],
        "limit": 0,
        "trace_filters": [dict(field="file_path", op="in", value=paths)],
    })[0]
    exs = [bigtrace.execution(None, e["queryUuid"]) for e in head["executions"]]
    tables = [q(ex["tableName"]) for ex in exs if ex["tableName"]]
    query = f"FROM {tables[0]}"
    if len(tables) > 1:  # The first row per trace of --metadata-query.
      query += (f" LEFT JOIN (FROM {tables[1]} QUALIFY row_number() OVER "
                "(PARTITION BY _trace_uuid) = 1) USING (_trace_uuid)")
    for row in bigtrace.sql(query).to_arrow_table().to_pylist():
      entry = todo[row.pop("_trace_uuid")]
      traces.append({
          **entry,
          **{
              k: v for k, v in row.items() if v is not None
          }
      })
    for ex in exs:
      if ex.get("errorMessage"):
        print(ex["errorMessage"], file=sys.stderr)
      bigtrace.delete(None, ex["queryUuid"])
  trace_dir = os.path.relpath(root, os.path.dirname(os.path.abspath(out)))
  with open(out, "w", encoding="utf-8") as f:
    json.dump({**old, "trace_dir": trace_dir, "traces": traces}, f, indent=2)


def call(server, path, body=None):
  """Calls the server; returns the response body and headers."""
  data = None if body is None else json.dumps(body).encode()
  request = urllib.request.Request(
      server.rstrip("/") + path, data, {"Content-Type": "application/json"})
  try:
    with urllib.request.urlopen(request) as response:
      return response.read(), response.headers
  except urllib.error.HTTPError as e:
    raise SystemExit(f"error: {json.load(e)['detail']}") from None
  except urllib.error.URLError as e:
    raise SystemExit(f"error: cannot reach {server}: {e.reason}") from None


def print_csv(args, path, body):
  data, headers = call(args.server, path, {**body, "format": "csv"})
  sys.stdout.buffer.write(data)
  if headers.get("X-Bigtrace-Truncated"):
    print(
        f"Showing the first {args.limit} rows; raise --limit, or aggregate in "
        "SQL.",
        file=sys.stderr)


def query_main(args):
  filters = []
  for f in args.filter:
    m = re.fullmatch(r"\s*(\w+)\s*(!=|<=|>=|=|<|>|glob)\s*(.*?)\s*", f)
    if not m:
      raise SystemExit(f"error: bad --filter {f!r}")
    filters.append(dict(zip(("field", "op", "value"), m.groups())))
  queries = [{
      "perfetto_sql": s,
      "table_name": t
  } for s, t in itertools.zip_longest(args.sql, args.table) if s]
  body = {
      "queries": queries,
      "limit": 0,
      "trace_filters": filters,
      "trace_limit": args.trace_limit
  }
  uuids = [
      e["queryUuid"] for e in json.loads(
          call(args.server, "/execute_bigtrace_query_async", body)[0])
      ["executions"]
  ]
  try:
    while True:
      statuses = [
          json.loads(call(args.server, f"/query_executions/{u}:status")[0])
          for u in uuids
      ]
      if all(s["status"] != "IN_PROGRESS" for s in statuses):
        break
      time.sleep(0.5)
  except KeyboardInterrupt:
    for u in uuids:
      call(args.server, f"/query_executions/{u}:cancel", {})
    return 130
  exit_code = 0
  for s in statuses:
    print(
        f"{s.get('tableName')}: {s['status']}, {s['processedRows']} rows "
        f"from {s['processedTraces']}/{s['totalTraces']} traces",
        file=sys.stderr)
    if s.get("errorMessage"):
      print(s["errorMessage"], file=sys.stderr)
    ok = s["status"] == "SUCCESS"
    exit_code = max(exit_code, 1 if not ok else 2 * bool(s.get("errorMessage")))
    if s.get("tableName"):
      print_csv(args, f"/query_executions/{s['queryUuid']}:fetch_results",
                {"limit": args.limit})
  return exit_code


def sql_main(args):
  print_csv(
      args, "/execute_bigtrace_query", {
          "engine": "duckdb",
          "materialized": False,
          "perfetto_sql": args.sql,
          "limit": args.limit
      })
