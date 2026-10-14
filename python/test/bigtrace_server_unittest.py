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
"""End-to-end tests of the local Bigtrace server (tools/bigtrace_server.py).

They run the real server and trace_processor_shell ($SHELL_PATH) on traces of
test/data.
"""

import contextlib
import json
import os
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock
import urllib.error
import urllib.request
import zipfile

from perfetto.bigtrace import cli
from perfetto.bigtrace.server import Bigtrace

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "tools", "bigtrace_server.py")
TRACES = [
    "api24_startup_cold.perfetto-trace", "api24_startup_hot.perfetto-trace",
    "api31_startup_warm.perfetto-trace"
]
COLD_SLICES = 9726  # Slices in api24_startup_cold.
ENDLESS = ("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n) "
           "SELECT count(1) FROM n")


def make_traces(tmp, names=TRACES):
  os.makedirs(tmp, exist_ok=True)
  for name in names:
    os.symlink(
        os.path.join(ROOT, "test", "data", name), os.path.join(tmp, name))
  return tmp


def free_port():
  with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    return s.getsockname()[1]


class Server:
  """tools/bigtrace_server.py serving `args` on a free port."""

  def __init__(self, *args):
    self.url = f"http://127.0.0.1:{free_port()}"
    self.proc = subprocess.Popen(
        [sys.executable, TOOL, *args, "-p",
         self.url.rsplit(":", 1)[1]],
        stdout=subprocess.DEVNULL)
    for _ in range(200):
      try:
        self.call("GET", "/")
        return
      except OSError:
        time.sleep(0.05)
    raise AssertionError("server did not start")

  def stop(self, sig=signal.SIGTERM):
    self.proc.send_signal(sig)
    self.proc.wait(10)

  def call(self, method, path, body=None, headers=None):
    """Returns (status, decoded body, headers)."""
    req = urllib.request.Request(
        self.url + path,
        None if body is None else json.dumps(body).encode(), {
            "Content-Type": "application/json",
            **(headers or {})
        },
        method=method)
    try:
      with urllib.request.urlopen(req) as res:
        data, status, hdrs = res.read(), res.status, res.headers
    except urllib.error.HTTPError as e:
      data, status, hdrs = e.read(), e.code, e.headers
    is_json = hdrs.get_content_type() == "application/json"
    return status, json.loads(data) if is_json else data, hdrs

  def ok(self, method, path, body=None):
    status, data, _ = self.call(method, path, body)
    assert status == 200, (status, data)
    return data

  def run(self, sql, table_name=None, **body):
    """Runs a trace query to the end; returns its execution."""
    queries = [{"perfetto_sql": sql, "table_name": table_name}]
    head = self.ok("POST", "/v1/execute_bigtrace_query_async", {
        "queries": queries,
        "limit": 0,
        **body
    })
    uuids = [e["queryUuid"] for e in head["executions"]]
    for _ in range(600):
      exs = [self.ok("GET", f"/v1/query_executions/{u}") for u in uuids]
      if all(e["status"] != "IN_PROGRESS" for e in exs):
        return exs
      time.sleep(0.1)
    raise AssertionError("query did not finish")

  def rows(self, ex, **body):
    page = self.ok("POST", f"/query_executions/{ex['queryUuid']}:fetch_results",
                   {
                       "limit": 1000,
                       **body
                   })
    names = [c["name"] for c in page["schema"]]
    return [dict(zip(names, r["values"])) for r in page["rows"]], page


def shells_of(pid):
  """PIDs of the trace_processor_shells started by process `pid`."""
  out = subprocess.run(["pgrep", "-f", "--", "--stdiod"],
                       capture_output=True,
                       text=True)
  shells = []
  for p in map(int, out.stdout.split()):
    try:
      with open(f"/proc/{p}/stat") as f:
        if f.read().split()[3] == str(pid):
          shells.append(p)
    except OSError:
      pass
  return shells


class ServerTest(unittest.TestCase):
  """API contract, end to end, over three good traces and a bad one."""

  @classmethod
  def setUpClass(cls):
    cls.tmp = tempfile.mkdtemp()
    make_traces(cls.tmp)
    with open(os.path.join(cls.tmp, "bad.perfetto-trace"), "w") as f:
      f.write("not a trace")
    cls.server = Server(cls.tmp)
    cls.good = {
        "filters": [{
            "field": "file_name",
            "op": "!=",
            "value": "bad.perfetto-trace"
        }]
    }

  @classmethod
  def tearDownClass(cls):
    cls.server.stop()
    shutil.rmtree(cls.tmp)

  def test_info_and_trace_list(self):
    s = self.server
    self.assertEqual(s.ok("GET", "/v1/")["features"], ["duckdb"])
    config = s.ok("POST", "/bigtrace_execution_config")["setting"]
    self.assertEqual([c["id"] for c in config][:3],
                     ["trace_directory", "recursive", "trace_manifest"])
    columns = s.ok("POST", "/trace_metadata_schema", {})["columns"]
    self.assertTrue({"link", "file_name", "file_size", "trace_uuid"} <=
                    {c["name"] for c in columns})
    page = s.ok(
        "POST", "/trace_metadata", {
            "columns": ["file_name", "file_size"],
            "filters": [{
                "field": "file_name",
                "op": "glob",
                "value": "api24*"
            }],
            "order_by": "file_size desc",
            "limit": 1,
        })
    self.assertEqual(page["totalFilteredRows"], 2)
    self.assertEqual(page["columnNames"], ["file_name", "file_size"])
    self.assertEqual(len(page["rows"]), 1)
    status, data, _ = s.call(
        "POST", "/trace_metadata", {
            "settings": [{
                "setting_id": "trace_directory",
                "values": ["/etc"],
                "category": "TRACE_ADDRESS"
            }]
        })
    self.assertEqual(status, 400, data)
    trace_uuid = s.ok("POST", "/trace_metadata",
                      {"columns": ["trace_uuid"]})["rows"][0]["values"][0]
    status, data, _ = s.call("GET", f"/trace/{trace_uuid}/x.perfetto-trace")
    self.assertEqual((status, len(data) > 1000), (200, True))

  def test_query_fetch_and_history(self):
    s = self.server
    ex, = s.run(
        "SELECT name, dur FROM slice",
        trace_metadata_columns=["file_name"],
        trace_filters=self.good["filters"],
        table_name="ignored")
    self.assertEqual(ex["status"], "SUCCESS")
    self.assertEqual(ex["processedTraces"], 3)
    self.assertGreater(ex["processedRows"], COLD_SLICES)
    self.assertEqual([c["name"] for c in ex["schema"]],
                     ["name", "dur", "file_name", "_trace_uuid"])
    rows, page = s.rows(
        ex,
        limit=2,
        offset=1,
        order_by="dur desc",
        filters=[{
            "field": "file_name",
            "op": "glob",
            "value": "*cold*"
        }])
    self.assertEqual(page["totalFilteredRows"], COLD_SLICES)
    self.assertEqual(len(rows), 2)
    self.assertGreaterEqual(int(rows[0]["dur"]), int(rows[1]["dur"]))
    self.assertIn("file_size", page["availableColumnNames"])
    rows, _ = s.rows(ex, columns=["file_size"], limit=1)
    self.assertEqual(list(rows[0]), ["file_size"])
    listed = s.ok("GET", "/query_executions")["queryExecutions"]
    self.assertIn(ex["queryUuid"], [e["queryUuid"] for e in listed])
    status = s.ok("GET", f"/query_executions/{ex['queryUuid']}:status")
    self.assertEqual(status["tableName"], ex["tableName"])
    s.ok("DELETE", f"/query_executions/{ex['queryUuid']}?drop_table=true")
    self.assertEqual(
        s.call("GET", f"/query_executions/{ex['queryUuid']}")[0], 404)
    tables = [t["name"] for t in s.ok("GET", "/duckdb/tables")["tables"]]
    self.assertNotIn(ex["tableName"], tables)

  def test_per_trace_errors(self):
    ex, = self.server.run("SELECT count(1) AS n FROM slice")
    self.assertEqual(ex["status"], "SUCCESS")
    self.assertRegex(ex["errorMessage"],
                     r"^1 trace\(s\) failed: bad.perfetto-trace: .*trace")
    self.assertEqual(ex["processedRows"], 3)
    ex, = self.server.run("SELECT * FROM no_such_table")
    self.assertEqual(ex["status"], "FAILED")
    self.assertIn("no_such_table", ex["errorMessage"])

  def test_multi_query_sync_and_naming(self):
    s = self.server
    body = {
        "queries": [{
            "perfetto_sql": "SELECT dur FROM slice LIMIT 2",
            "table_name": "durs"
        }, {
            "perfetto_sql": "SELECT 1 AS a, 2 AS a"
        }],
        "limit": 5,
        **{
            "trace_filters": self.good["filters"]
        }
    }
    res = s.ok("POST", "/execute_bigtrace_query", body)
    self.assertEqual(len(res["rows"]), 5)  # The row limit spans traces.
    self.assertEqual(res["tableName"], "durs")
    second = res["executions"][1]
    self.assertRegex(second["tableName"], r"^result_\d+$")
    rows, _ = s.rows(second)
    self.assertEqual([c for c in rows[0]][:2], ["a", "a_2"])
    check = s.ok("POST", "/check_table_exists", {"table_name": "du rs"})
    self.assertEqual((check["exists"], check["resolvedTableName"]),
                     (False, "du_rs"))
    check = s.ok("POST", "/check_table_exists", {"table_name": "durs"})
    self.assertEqual((check["exists"], check["suggestedTableName"]),
                     (True, "durs_1"))

  def test_type_widening(self):
    sql = (f"SELECT IIF((SELECT count(1) FROM slice) = {COLD_SLICES}, {{}}, 2) "
           "AS v")
    for value, bt_type, values in (("1.5", "FLOAT64", ["1.5", "2.0", "2.0"]),
                                   ("'x'", "STRING", ["2", "2", "x"])):
      ex, = self.server.run(
          sql.format(value), trace_filters=self.good["filters"])
      self.assertEqual(ex["schema"][0], {"name": "v", "type": bt_type})
      rows, _ = self.server.rows(ex)
      self.assertEqual(sorted(r["v"] for r in rows), values)

  def test_cancel(self):
    s = self.server
    head = s.ok("POST", "/execute_bigtrace_query_async", {
        "perfetto_sql": ENDLESS,
        **self.good
    })
    time.sleep(1)
    self.assertTrue(shells_of(s.proc.pid))
    s.ok("POST", f"/query_executions/{head['queryUuid']}:cancel", {})
    for _ in range(50):
      ex = s.ok("GET", f"/query_executions/{head['queryUuid']}")
      if ex.get("endTime") and not shells_of(s.proc.pid):
        break
      time.sleep(0.1)
    self.assertEqual(ex["status"], "CANCELLED")
    self.assertEqual(shells_of(s.proc.pid), [])

  def test_duckdb_mode(self):
    s = self.server
    ex, = s.run(
        "SELECT count(1) AS n FROM slice",
        table_name="counts",
        trace_filters=self.good["filters"])
    sql = ("SELECT m.file_name, c.n FROM counts c JOIN trace_metadata m "
           "USING (_trace_uuid) ORDER BY n DESC")
    res = s.ok("POST", "/execute_bigtrace_query", {
        "engine": "duckdb",
        "materialized": False,
        "perfetto_sql": sql
    })
    self.assertEqual(len(res["rows"]), 3)
    self.assertEqual([c["type"] for c in res["schema"]], ["STRING", "INT64"])
    self.assertEqual(
        s.ok(
            "POST", "/execute_bigtrace_query", {
                "engine": "duckdb",
                "perfetto_sql": "FROM latest",
                "limit": 0,
                "materialized": False
            })["totalFilteredRows"], 3)
    res = s.ok("POST", "/execute_bigtrace_query", {
        "engine": "duckdb",
        "perfetto_sql": sql,
        "table_name": "by_file"
    })
    self.assertEqual(res["tableName"], "by_file")
    status, data, _ = s.call(
        "POST", "/execute_bigtrace_query", {
            "engine": "duckdb",
            "materialized": False,
            "perfetto_sql": "SELECT * FROM read_csv('/etc/passwd')"
        })
    self.assertEqual(status, 400, data)
    head = s.ok(
        "POST", "/execute_bigtrace_query_async", {
            "engine": "duckdb",
            "materialized": False,
            "perfetto_sql": "SELECT sum(i) FROM range(10000000000) t(i)"
        })
    time.sleep(0.3)
    s.ok("POST", f"/query_executions/{head['queryUuid']}:cancel", {})
    time.sleep(0.5)
    ex = s.ok("GET", f"/query_executions/{head['queryUuid']}")
    self.assertEqual(ex["status"], "CANCELLED")

  def test_cli(self):
    run = lambda *a: subprocess.run(
        [sys.executable, TOOL, *a, "--server", self.server.url],
        capture_output=True,
        text=True)
    res = run("query", "SELECT name FROM slice", "--table", "names", "--filter",
              "file_name glob api24*", "--limit", "3")
    self.assertEqual(res.returncode, 0, res.stderr)
    self.assertEqual(res.stdout.splitlines()[0], '"name","_trace_uuid"')
    self.assertEqual(len(res.stdout.splitlines()), 4)
    self.assertIn("names: SUCCESS", res.stderr)
    self.assertIn("first 3 rows", res.stderr)
    res = run("query", "SELECT 1 AS one")  # bad.perfetto-trace fails.
    self.assertEqual(res.returncode, 2, res.stderr)
    self.assertIn("1 trace(s) failed", res.stderr)
    res = run("sql", "SELECT count(DISTINCT _trace_uuid) AS n FROM names")
    self.assertEqual((res.returncode, res.stdout), (0, '"n"\n2\n'))
    res = run("sql", "SELECT * FROM nope")
    self.assertEqual(res.returncode, 1)
    self.assertIn("nope", res.stderr)

  def test_http_robustness(self):
    s = self.server
    self.assertEqual(s.call("GET", "/", headers={"Host": "evil.com"})[0], 403)
    self.assertEqual(
        s.call("GET", "/", headers={"Origin": "https://evil.com"})[0], 403)
    status, _, headers = s.call(
        "GET", "/", headers={"Origin": "https://ui.perfetto.dev"})
    self.assertEqual(headers["Access-Control-Allow-Origin"],
                     "https://ui.perfetto.dev")
    self.assertEqual(
        s.call("POST", "/refresh", {}, {"Content-Type": "text/plain"})[0], 415)
    self.assertEqual(s.call("GET", "/nope")[0], 404)
    self.assertEqual(s.call("POST", "/query_executions")[0], 405)
    self.assertEqual(s.call("GET", "/query_executions/nope:status")[0], 404)
    self.assertEqual(
        s.call(
            "POST", "/trace_metadata",
            {"filters": [{
                "field": "file_name",
                "op": "like",
                "value": "x"
            }]})[0], 400)


class LifecycleTest(unittest.TestCase):
  """Persistence, manifests, archives and shells dying with the server."""

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.traces = make_traces(os.path.join(self.tmp, "traces"))

  def tearDown(self):
    shutil.rmtree(self.tmp)

  def bigtrace(self, traces=None, manifest=None, db=None):
    return Bigtrace(traces or self.traces, False, manifest,
                    os.environ["SHELL_PATH"], 4, db, "http://h", "https://ui")

  def test_history_persists(self):
    db = os.path.join(self.tmp, "bt.duckdb")
    server = Server(self.traces, "--duckdb-path", db)
    ex, = server.run("SELECT count(1) AS n FROM slice", table_name="kept")
    server.stop()
    server = Server(self.traces, "--duckdb-path", db)
    self.assertEqual(
        server.ok("GET", f"/query_executions/{ex['queryUuid']}"), ex)
    self.assertEqual(len(server.rows(ex)[0]), 3)
    server.stop()

  def test_shells_die_with_server(self):
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGKILL):
      server = Server(self.traces)
      server.ok("POST", "/execute_bigtrace_query_async",
                {"perfetto_sql": ENDLESS})
      time.sleep(1)
      shells = shells_of(server.proc.pid)
      self.assertEqual(len(shells), 3)
      server.stop(sig)
      time.sleep(0.5)
      self.assertFalse([p for p in shells if os.path.exists(f"/proc/{p}")], sig)

  def test_manifests(self):
    traces = [{
        "file_path": t,
        "device": f"d{i}",
        "experiment_id": 1,
        "control_id": 2,
        "is_treatment": i % 2 == 0
    } for i, t in enumerate(TRACES)]
    json_manifest = os.path.join(self.tmp, "m.json")
    with open(json_manifest, "w") as f:
      json.dump(
          {
              "trace_dir":
                  "traces",
              "traces":
                  traces,
              "schema": [{
                  "name": "device",
                  "displayName": "Device"
              }],
              "presets": [{
                  "id": "p"
              }],
              "experiments": [{
                  "experiment_id": 1,
                  "experiment_name": "Exp",
                  "control_id": 2,
                  "control_name": "Ctl"
              }]
          }, f)
    csv_manifest = os.path.join(self.traces, "m.csv")
    with open(csv_manifest, "w") as f:
      f.write("file_path,device,experiment_id,control_id,is_treatment\n")
      f.writelines(f"{t['file_path']},{t['device']},1,2,{t['is_treatment']}\n"
                   for t in traces)
    db_manifest = os.path.join(self.traces, "metadata.db")
    with contextlib.closing(sqlite3.connect(db_manifest)) as conn, conn:
      conn.execute("CREATE TABLE traces (absolute_local_file_path, device, "
                   "experiment_id, control_id, is_treatment)")
      conn.executemany("INSERT INTO traces VALUES (?, ?, ?, ?, ?)",
                       [(os.path.join(self.traces, t["file_path"]), t["device"],
                         1, 2, t["is_treatment"]) for t in traces])
    for manifest in (json_manifest, csv_manifest, None):  # None: metadata.db.
      bt = self.bigtrace(manifest=manifest)
      page = bt.trace_list({
          "columns": ["device", "file_size"],
          "experiment_filter": {
              "experiment_id": 1,
              "control_id": 2,
              "is_treatment": True
          },
          "order_by": "device"
      })
      self.assertEqual([r["values"][0] for r in page["rows"]], ["d0", "d2"])
      self.assertTrue(all(int(r["values"][1]) > 0 for r in page["rows"]))
    bt = self.bigtrace(manifest=json_manifest)
    schema = {c["name"]: c for c in bt.metadata_schema()}
    self.assertEqual(schema["device"]["displayName"], "Device")
    self.assertEqual(bt.trace_presets(None), {"tracePresets": [{"id": "p"}]})
    found = bt.experiments_search({"search_query": "ct"})["experiments"]
    self.assertEqual(found[0]["experimentName"], "Exp")
    self.assertEqual(bt.experiment({"experiment_id": 3}), {})

  def test_generate_manifest(self):
    bt = self.bigtrace()
    out = os.path.join(self.traces, "bigtrace_manifest.json")
    cli.generate_manifest(bt, out, False, "SELECT 7 AS seven")
    with open(out) as f:
      traces = json.load(f)["traces"]
    self.assertEqual(len(traces), 3)
    cold = next(t for t in traces if "cold" in t["file_path"])
    self.assertEqual((cold["slice_count"], cold["seven"]), (COLD_SLICES, 7))
    self.assertIn("trace_uuid", cold)
    cold["seven"] = 8
    with open(out, "w") as f:
      json.dump({"traces": traces}, f)
    cli.generate_manifest(bt, out, False, None)
    with open(out) as f:
      self.assertIn(8, [t["seven"] for t in json.load(f)["traces"]])
    bt.load_catalog(self.traces, False, out)
    self.assertEqual(bt.info(None)["traceCount"], 3)

  def test_archives_and_exports(self):
    archive = os.path.join(self.tmp, "traces.zip")
    with zipfile.ZipFile(archive, "w") as z:
      z.write(os.path.join(self.traces, TRACES[0]), f"sub/{TRACES[0]}")
    with unittest.mock.patch.object(cli, "CACHE_DIR", self.tmp):
      unpacked = cli.unpack(archive)
      self.assertEqual(cli.unpack(archive), unpacked)  # Extracted once.
    exports = os.path.join(self.tmp, "exports")
    os.mkdir(exports)
    export = os.path.join(exports, "cold.perfetto.tar")
    subprocess.run([
        os.environ["SHELL_PATH"], "--quiet", "export", "perfetto", "-o", export,
        os.path.join(self.traces, TRACES[0])
    ],
                   check=True,
                   capture_output=True)
    if shutil.which("zstd"):
      subprocess.run(["zstd", "-q", "--rm", export], check=True)
    server = Server(unpacked, "-r")
    self.assertEqual(server.ok("GET", "/")["traceCount"], 1)
    server.stop()
    server = Server(exports)
    ex, = server.run("SELECT count(1) AS n FROM slice")
    self.assertEqual(server.rows(ex)[0][0]["n"], str(COLD_SLICES))
    server.stop()


def varint(n):
  out = b""
  while n >= 0x80:
    out += bytes([n & 0x7F | 0x80])
    n >>= 7
  return out + bytes([n])


def field(number, data):
  return varint(number << 3 | 2) + varint(len(data)) + data


def reply(columns, cells, varints=(), strings=(), blobs=(), doubles=b""):
  """A TraceProcessorRpc frame holding a QueryResult with one batch."""
  batch = field(1, bytes(cells)) + field(
      2, b"".join(varint(v % (1 << 64)) for v in varints)) + field(3, doubles)
  batch += field(5, b"".join(s + b"\0" for s in strings))
  batch += b"".join(field(4, b) for b in blobs) + b"\x30\x01"
  result = b"".join(field(1, c.encode()) for c in columns) + field(3, batch)
  return field(1, b"\x18\x03" + field(203, result))


class ExtensionProtocolTest(unittest.TestCase):
  """The extension against hand-made (and malformed) shell replies."""

  def test_replies(self):
    tmp = tempfile.mkdtemp()
    self.addCleanup(shutil.rmtree, tmp)
    shell = os.path.join(tmp, "fake_shell")
    with open(shell, "w") as f:  # Replies with the bytes of its "trace".
      f.write(f"#!{sys.executable}\nimport sys\nsys.stdin.buffer.read(1)\n"
              "sys.stdout.buffer.write(open(sys.argv[-1], 'rb').read())\n")
    os.chmod(shell, 0o755)
    cols = ["i", "s", "b"]
    good = reply(
        cols, [2, 4, 5, 1, 1, 3],
        varints=[-1],
        strings=[b"caf\xc3\xa9\xff"],
        blobs=[b"\0\1"],
        doubles=b"\0\0\0\0\0\0\xf8\x3f")
    bad = {  # Fed in this order, with the column names of the first.
        "bad_cell_type": reply(cols, [9, 1, 1]),
        "cells_mismatch": reply(cols, [2, 2, 2], varints=[1]),
        "good": good,
        "huge_frame": varint(10) + varint(1 << 40),
        "truncated": good[:len(good) // 2],
        "other_columns": reply(["x"], [1]),
    }
    traces = make_traces(os.path.join(tmp, "traces"), [])
    for name, data in bad.items():
      with open(os.path.join(traces, name), "wb") as f:
        f.write(data)
    bt = Bigtrace(traces, False, None, shell, 1, None, "http://h", "https://ui")
    head, _, _ = bt.execute({
        "perfetto_sql": "x",
        "limit": 100,
        "trace_order_by": "file_name"
    })
    ex = bt.executions[head["queryUuid"]]
    self.assertEqual(ex["status"], "SUCCESS")
    errors = ex["errorMessage"]
    self.assertTrue(errors.startswith("5 trace(s) failed"), errors)
    page = bt.fetch(ex["queryUuid"], {"limit": 10})[1].to_pylist()
    self.assertEqual(len(page), 2)
    row = next(r for r in page if r["i"] is not None)
    self.assertEqual((row["i"], row["s"], row["b"]),
                     (-1, "caf" + "\ufffd" * 3, "AAE="))
    other = next(r for r in page if r["i"] is None)
    self.assertEqual((other["s"], other["b"]), (None, "1.5"))


if __name__ == "__main__":
  unittest.main()
