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
"""Failure handling tests: crashing shells, bad files, full disks, shutdown."""

import concurrent.futures
import contextlib
import io
import os
import signal
import sqlite3
import stat
import sys
import tempfile
import time
import unittest
from unittest import mock

from perfetto.bigtrace.server import (
    BigtraceQueryExecutor,
    TraceCatalog,
    resolve_trace_processor_bin,
)
from perfetto.bigtrace.server.cli import parse_args
from perfetto.trace_processor.api import TraceProcessorConfig
from test.bigtrace_server_fixture import (
    COLD_TRACE,
    ENDLESS_SQL,
    HOT_TRACE,
    WARM_TRACE,
    base_url,
    copy_trace,
    request,
    running_server,
    wait_for,
)

TRACES = {"cold.pftrace": COLD_TRACE, "hot.pftrace": HOT_TRACE}


def _write_script(path: str, body: str) -> str:
  with open(path, "w", encoding="utf-8") as f:
    f.write("#!/bin/sh\n" + body)
  os.chmod(path, stat.S_IRWXU)
  return path


def _child_shells():
  """Returns pids of this process's trace_processor_shell children."""
  pids = []
  for pid in filter(str.isdigit, os.listdir("/proc")):
    try:
      with open(f"/proc/{pid}/stat", encoding="utf-8") as f:
        comm, rest = f.read().rsplit(")", 1)
    except OSError:
      continue
    if int(rest.split()[1]) == os.getpid() and "trace_processor" in comm:
      pids.append(int(pid))
  return pids


class ExecutorRobustnessTest(unittest.TestCase):
  """Runs the executor over a temporary copy of three traces."""

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.dir = tmp.name
    self.paths = {
        name: copy_trace(trace, self.dir, name)
        for name, trace in (("cold.pftrace", COLD_TRACE),
                            ("hot.pftrace", HOT_TRACE), ("warm.pftrace",
                                                         WARM_TRACE))
    }
    self.executor = BigtraceQueryExecutor(
        catalog=TraceCatalog(trace_dir=self.dir),
        tp_config=TraceProcessorConfig(bin_path=resolve_trace_processor_bin()),
        max_workers=3)
    self.addCleanup(self.executor.close)

  def _run(self, sql, **params):
    return self.executor.execute({"perfetto_sql": sql, **params})

  def _rows_by_file(self, result):
    names = [c["name"] for c in result["schema"]]
    i, f = names.index("n"), names.index("file_name")
    return {r["values"][f]: r["values"][i] for r in result["rows"]}

  def _wait_until_done(self, uid):
    wait_for(lambda: self.executor.get_status(uid)["status"] != "IN_PROGRESS")
    return self.executor.get_execution(uid)

  def test_shell_killed_mid_query_fails_only_that_trace(self):
    counts = self._rows_by_file(
        self._run(
            "SELECT count(*) AS n FROM slice",
            trace_metadata_columns=["file_name"]))
    self.assertEqual(len(set(counts.values())), 3)
    # Endless on cold only.
    sql = ("WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n "
           "WHERE i < IIF((SELECT count(*) FROM slice) = "
           f"{counts['cold.pftrace']}, 1e18, 0)) SELECT count(*) AS n FROM n")
    uid = self.executor.execute({"perfetto_sql": sql},
                                is_async=True)["queryUuid"]
    ex = self.executor._executions[uid]
    wait_for(lambda: ex["processedTraces"] == 2 and len(ex["_active"]) == 1)
    (tp,) = list(ex["_active"])
    os.kill(tp.subprocess.pid, signal.SIGKILL)

    ex = self._wait_until_done(uid)
    self.assertEqual(ex["status"], "SUCCESS")
    self.assertEqual(ex["processedRows"], 2)
    self.assertRegex(ex["errorMessage"], r"^1 trace\(s\) failed: cold.pftrace")
    self.assertEqual(self._run("SELECT 1 AS n")["totalFilteredRows"], 3)

  def test_bad_files_are_reported_per_trace(self):
    open(os.path.join(self.dir, "empty.pftrace"), "wb").close()
    with open(self.paths["cold.pftrace"], "rb") as src:
      head = src.read(20000)
    with open(os.path.join(self.dir, "truncated.pftrace"), "wb") as f:
      f.write(head)
    with open(os.path.join(self.dir, "notes.txt"), "w", encoding="utf-8") as f:
      f.write("not a trace\n")
    self.executor.catalog.reload()

    result = self._run(
        "SELECT count(*) AS n FROM slice", trace_metadata_columns=["file_name"])
    counts = self._rows_by_file(result)
    # Empty and truncated files load as (nearly) empty traces.
    self.assertEqual(counts["empty.pftrace"], "0")
    self.assertIn("truncated.pftrace", counts)
    self.assertNotIn("notes.txt", counts)
    ex = self.executor.get_execution(result["queryUuid"])
    self.assertEqual(ex["status"], "SUCCESS")
    self.assertRegex(ex["errorMessage"],
                     r"^1 trace\(s\) failed: notes.txt: .*Unknown trace type")

  def test_trace_deleted_after_listing_is_reported(self):
    os.remove(self.paths["hot.pftrace"])
    result = self._run("SELECT 1 AS n")
    self.assertEqual(result["totalFilteredRows"], 2)
    ex = self.executor.get_execution(result["queryUuid"])
    self.assertEqual(ex["errorMessage"],
                     "1 trace(s) failed: hot.pftrace: trace file not found")

  def test_query_failing_everywhere_raises(self):
    with self.assertRaisesRegex(Exception, "3 trace\\(s\\) failed"):
      self._run("SELECT * FROM no_such_table")

  def test_history_write_failure_still_serves_results(self):
    disk_full = OSError("database or disk is full")
    with mock.patch.object(
        self.executor.duckdb, "save_execution", side_effect=disk_full), \
        self.assertLogs("perfetto.bigtrace.server.executor", "WARNING") as logs:
      result = self._run("SELECT 1 AS n")
    self.assertIn("database or disk is full", logs.output[0])
    self.assertEqual(result["totalFilteredRows"], 3)
    page = self.executor.fetch_results(result["queryUuid"], limit=2)
    self.assertEqual(len(page["rows"]), 2)
    self.assertEqual(self._run("SELECT 2 AS n")["totalFilteredRows"], 3)

  def test_deleting_a_running_query_does_not_bring_it_back(self):
    uid = self.executor.execute({"perfetto_sql": ENDLESS_SQL},
                                is_async=True)["queryUuid"]
    ex = self.executor._executions[uid]
    wait_for(lambda: len(ex["_active"]) == 3)
    self.assertTrue(self.executor.delete_execution(uid))
    for run in self.executor._runs:
      run.join(timeout=10)
    self.assertIsNone(self.executor.get_execution(uid))
    self.assertNotIn(uid,
                     [e["queryUuid"] for e in self.executor.list_executions()])

  def test_concurrent_queries_get_their_own_results(self):
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as threads:
      results = list(
          threads.map(lambda i: self._run(f"SELECT {i} AS n"), range(6)))
    for i, result in enumerate(results):
      self.assertEqual([r["values"][0] for r in result["rows"]], [str(i)] * 3)

  def test_paging_sorting_and_filtering_results(self):
    result = self._run(
        "SELECT id AS n FROM slice ORDER BY id LIMIT 3",
        trace_metadata_columns=["file_name"])
    uid = result["queryUuid"]
    fetch = lambda **kw: self.executor.fetch_results(uid, **kw)

    page = fetch(order_by="n desc, file_name", limit=4)
    values = [int(r["values"][0]) for r in page["rows"]]
    self.assertEqual(values, sorted(values, reverse=True))
    self.assertEqual(page["totalFilteredRows"], 9)

    hot = [{"field": "file_name", "op": "=", "value": "hot.pftrace"}]
    page = fetch(filters=hot, order_by="n", offset=2, limit=5, columns=["n"])
    self.assertEqual(page["totalFilteredRows"], 3)
    self.assertEqual([s["name"] for s in page["schema"]], ["n"])
    self.assertEqual(len(page["rows"]), 1)

    # Rows appended to the DuckDB table are immediately reflected in paging.
    # Result tables store only `_trace_uuid` + query columns; trace metadata
    # is joined in when paging.
    tname = self.executor._executions[uid]["tableName"]
    duck = self.executor.duckdb
    cols = [c["name"] for c in duck._table_meta[tname]["columns"]]
    self.assertEqual(sorted(cols), ["_trace_uuid", "n"])
    uuid = duck._conn.execute(
        f"SELECT CAST(_trace_uuid AS VARCHAR) FROM {tname} LIMIT 1").fetchone(
        )[0]
    duck._conn.executemany(
        f"INSERT INTO {tname} (_trace_uuid, n) VALUES (?, ?)", [(uuid, 10),
                                                                (uuid, 11),
                                                                (uuid, 12)])
    self.assertEqual(fetch(order_by="n desc")["totalFilteredRows"], 12)


class ServerFailureTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.dir = tmp.name
    for name, trace in TRACES.items():
      copy_trace(trace, self.dir, name)

  def test_unusable_shell_fails_queries_but_server_keeps_running(self):
    bin_dir = tempfile.TemporaryDirectory()
    self.addCleanup(bin_dir.cleanup)
    broken = _write_script(
        os.path.join(bin_dir.name, "broken_shell"),
        "echo shell is broken >&2\nexit 1\n")
    for bin_path, error in ((broken, "shell is broken"),
                            (os.path.join(bin_dir.name, "missing"),
                             "Path to binary is not valid")):
      with self.subTest(bin_path), running_server(
          trace_dir=self.dir, bin_path=bin_path) as server:
        url = base_url(server)
        status, data, _ = request(url, "POST", "/execute_bigtrace_query",
                                  {"perfetto_sql": "SELECT 1"})
        self.assertEqual(status, 400)
        self.assertIn("2 trace(s) failed", data["detail"])
        self.assertIn(error, data["detail"])
        self.assertEqual(request(url, "GET", "/")[0], 200)

  def test_missing_shell_path_flag_is_rejected(self):
    with self.assertRaises(SystemExit), contextlib.redirect_stderr(
        io.StringIO()):
      parse_args(["--shell-path", os.path.join(self.dir, "missing")])

  @unittest.skipUnless(sys.platform.startswith("linux"), "uses /proc")
  def test_stop_is_prompt_and_leaves_no_shells(self):
    server_cm = running_server(trace_dir=self.dir, max_workers=2)
    server = server_cm.__enter__()
    try:
      url = base_url(server)
      request(url, "POST", "/execute_bigtrace_query",
              {"perfetto_sql": "SELECT 1"})
      uid = request(url, "POST", "/execute_bigtrace_query_async",
                    {"perfetto_sql": ENDLESS_SQL})[1]["queryUuid"]
      ex = server.executor._executions[uid]
      wait_for(lambda: len(ex["_active"]) == 2)
      self.assertTrue(_child_shells())
    finally:
      start = time.monotonic()
      server_cm.__exit__(None, None, None)
    self.assertLess(time.monotonic() - start, 5)
    self.assertEqual(_child_shells(), [])


if __name__ == "__main__":
  unittest.main()
