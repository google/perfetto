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
    SnapshotStore,
    TraceCatalog,
    TraceProcessorPool,
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
        max_workers=3,
        pool_size=3)
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
    self.assertFalse(self.executor.pool.is_loaded(self.paths["cold.pftrace"]))
    self.assertEqual(self._run("SELECT 1 AS n")["totalFilteredRows"], 3)

  def test_shell_that_died_while_idle_is_replaced(self):
    self._run("SELECT 1 AS n")
    hot = self.paths["hot.pftrace"]
    idle = self.executor.pool._idle[hot]
    idle.subprocess.kill()
    idle.subprocess.wait()
    result = self._run("SELECT count(*) AS n FROM slice")
    self.assertEqual(result["totalFilteredRows"], 3)
    self.assertIsNot(self.executor.pool._idle[hot], idle)

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
    disk_full = sqlite3.OperationalError("database or disk is full")
    db = self.executor.history_db
    with mock.patch.object(db, "save_execution", side_effect=disk_full), \
        mock.patch.object(db, "save_results", side_effect=disk_full), \
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

    # Rows that arrive later invalidate the cached sorted view.
    rows = self.executor._executions[uid]["_rows"]
    batch, columns, count, meta = rows._chunks[0]
    rows.add([(batch, count)], columns, meta)
    self.assertEqual(fetch(order_by="n desc")["totalFilteredRows"], 12)


class SessionTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.dir = tmp.name
    for name, trace in TRACES.items():
      copy_trace(trace, self.dir, name)
    server_cm = running_server(
        trace_dir=self.dir,
        pool_size=1,
        max_workers=1,
        snapshot_dir=os.path.join(self.dir, "snapshots"))
    self.server = server_cm.__enter__()
    self.addCleanup(server_cm.__exit__, None, None, None)
    self.session = self.server.executor.pool._session

  def _query(self, sql):
    return request(
        base_url(self.server), "POST", "/execute_bigtrace_query", {
            "perfetto_sql": sql,
            "limit": 100
        })

  def test_only_statements_without_output_are_recorded(self):
    for sql in ("SELECT 1", "SELECT * FROM slice WHERE 0",
                "CREATE PERFETTO TABLE bad AS SELECT * FROM no_such_table"):
      self._query(sql)
    self.assertEqual(self.session, [])
    self._query("INCLUDE PERFETTO MODULE android.startup.startups")
    self.assertEqual(len(self.session), 1)

  def test_session_replays_on_traces_loaded_from_snapshots(self):
    self._query("SELECT 1")
    self.server.snapshots.wait()
    for name in TRACES:
      self.assertTrue(self.server.snapshots.find(os.path.join(self.dir, name)))
    status, _, _ = self._query(
        "CREATE PERFETTO TABLE t AS SELECT count(*) AS c FROM slice")
    self.assertEqual(status, 200)
    # One instance is kept, so each query reloads the other trace from its
    # snapshot and replays the CREATE there.
    for _ in range(2):
      status, data, _ = self._query("SELECT c FROM t")
      self.assertEqual(status, 200, data)
      self.assertEqual(len(data["rows"]), 2)
    self.assertEqual(len(self.session), 1)


class SnapshotFailureTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.dir = tmp.name
    self.trace = copy_trace(COLD_TRACE, self.dir, "cold.pftrace")
    self.snapshot_dir = os.path.join(self.dir, "snapshots")
    self.bin_path = resolve_trace_processor_bin()

  def _query_twice(self, store):
    """Queries the trace twice; returns whether either used a snapshot."""
    pool = TraceProcessorPool(
        TraceProcessorConfig(bin_path=self.bin_path), 0, store)
    self.addCleanup(pool.close)
    from_snapshot = []
    for _ in range(2):
      pool.start_query("SELECT 1")
      tp = pool.acquire(self.trace)
      from_snapshot.append(pool.loaded_from_snapshot(tp))
      self.assertEqual(list(tp.query("SELECT 1 AS x"))[0].x, 1)
      pool.release(self.trace, tp)
      pool.end_query()
      store.wait()
    return any(from_snapshot)

  def test_disk_full_while_compressing_leaves_no_files(self):
    store = SnapshotStore(self.snapshot_dir, 2**30, self.bin_path)
    self.addCleanup(store.close)
    # Writes part of its output, then fails like a full disk.
    store._zstd = _write_script(
        os.path.join(self.dir, "zstd"), 'for last; do :; done\n'
        'echo partial > "$last"\n'
        'echo "No space left on device" >&2\nexit 1\n')
    with self.assertLogs("perfetto.bigtrace.server.snapshot",
                         "WARNING") as logs:
      self.assertFalse(self._query_twice(store))
    self.assertIn("No space left on device", logs.output[0])
    self.assertEqual(os.listdir(self.snapshot_dir), [])

  def test_unwritable_snapshot_dir_only_disables_snapshots(self):
    store = SnapshotStore(self.snapshot_dir, 2**30, self.bin_path)
    self.addCleanup(store.close)
    os.chmod(self.snapshot_dir, stat.S_IRUSR | stat.S_IXUSR)
    self.addCleanup(os.chmod, self.snapshot_dir, stat.S_IRWXU)
    with self.assertLogs("perfetto.bigtrace.server.snapshot", "WARNING"):
      self.assertFalse(self._query_twice(store))

  def test_smaller_budget_evicts_on_startup(self):
    store = SnapshotStore(self.snapshot_dir, 2**30, self.bin_path)
    hot = copy_trace(HOT_TRACE, self.dir, "hot.pftrace")
    store.create(self.trace)
    os.utime(store.find(self.trace), (1, 1))  # The least recently used.
    store.create(hot)
    store.close()
    store = SnapshotStore(self.snapshot_dir, 1, self.bin_path)
    self.addCleanup(store.close)
    self.assertIsNone(store.find(self.trace))
    self.assertIsNotNone(store.find(hot))
    self.assertEqual(len(os.listdir(self.snapshot_dir)), 1)


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
      with self.subTest(bin_path), self.assertLogs(
          "perfetto.bigtrace.server.server", "WARNING"), running_server(
              trace_dir=self.dir,
              bin_path=bin_path,
              snapshot_dir=os.path.join(self.dir, "snapshots")) as server:
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
    server_cm = running_server(
        trace_dir=self.dir,
        pool_size=2,
        max_workers=2,
        snapshot_dir=os.path.join(self.dir, "snapshots"))
    server = server_cm.__enter__()
    try:
      url = base_url(server)
      # Loads both traces and queues their snapshots, which wait for the
      # endless query below to finish.
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
