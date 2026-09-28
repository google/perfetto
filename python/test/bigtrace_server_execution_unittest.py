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
"""Query execution, history and pool tests for the Bigtrace server."""

import concurrent.futures
import os
import tempfile
import threading
import unittest

from perfetto.bigtrace.server import (
    BigtraceHistoryDb,
    EphemeralTraceProcessorPool,
    KeepAliveTraceProcessorPool,
    resolve_trace_processor_bin,
)
from perfetto.bigtrace.server.filters import sort_records
from perfetto.trace_processor.api import (
    TraceProcessorConfig,
    TraceProcessorException,
)
from test.bigtrace_server_fixture import (
    BigtraceServerTestCase,
    COLD_TRACE,
    HOT_TRACE,
    MANIFEST_JSON,
    TEST_DATA_DIR,
    WARM_TRACE,
    base_url,
    copy_trace,
    request,
    running_server,
    trace_path,
)


class BigtraceServerExecutionTest(BigtraceServerTestCase):

  def test_concurrent_mixed_requests_all_succeed(self):
    requests = [
        ("GET", "/", None),
        ("HEAD", "/", None),
        ("POST", "/trace_metadata", {
            "limit": 5,
            "offset": 0
        }),
        ("POST", "/trace_metadata_schema", {}),
        ("GET", "/trace_presets", None),
        ("GET", "/query_executions", None),
        ("POST", "/check_table_exists", {
            "table_name": "test_table"
        }),
        ("POST", "/execute_bigtrace_query", {
            "perfetto_sql": "SELECT 1 AS n;",
            "limit": 5
        }),
    ]
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
      futures = [
          pool.submit(self._request, *requests[i % len(requests)])
          for i in range(25)
      ]
      for future in concurrent.futures.as_completed(futures):
        self.assertEqual(future.result()[0], 200)

  def test_negative_paging_is_clamped(self):
    status, data, _ = self._request("POST", "/trace_metadata", {
        "limit": -10,
        "offset": -5
    })
    self.assertEqual(status, 200)
    self.assertIn("rows", data)

  def test_offset_past_end_returns_no_rows(self):
    status, data, _ = self._request("POST", "/trace_metadata", {
        "limit": 10,
        "offset": 999999
    })
    self.assertEqual(status, 200)
    self.assertEqual(data.get("rows", []), [])

  def test_unknown_order_by_columns_are_ignored(self):
    status, data, _ = self._request("POST", "/trace_metadata", {
        "order_by": "definitely_not_a_real_col desc, 12345, empty_val",
        "limit": 10
    })
    self.assertEqual(status, 200)
    self.assertIn("rows", data)

  def test_unknown_filter_operator_is_rejected(self):
    status, data, _ = self._request(
        "POST", "/trace_metadata", {
            "filters": [{
                "field": "fake_column",
                "op": "unknown_operator",
                "value": "val"
            }]
        })
    self.assertEqual(status, 400)
    self.assertIn("detail", data)

  def test_fetch_results_of_unknown_query_returns_404(self):
    status, data, _ = self._request(
        "POST", "/query_executions/non-existent-uuid:fetch_results", {})
    self.assertEqual(status, 404)
    self.assertIn("detail", data)

  def test_redundant_slashes_and_v1_prefix_are_normalized(self):
    status, _, _ = self._request("POST", "///v1///trace_metadata///",
                                 {"limit": 2})
    self.assertEqual(status, 200)

  def test_cancel_and_delete_of_unknown_query_succeed(self):
    status, _, _ = self._request("POST", "/query_executions/ghost-uuid:cancel",
                                 {})
    self.assertEqual(status, 200)
    status, _, _ = self._request("DELETE", "/query_executions/ghost-uuid")
    self.assertEqual(status, 200)

  def test_cancel_after_finish_keeps_success_status(self):
    _, data, _ = self._request("POST", "/execute_bigtrace_query", {
        "perfetto_sql": "SELECT 1 AS val;",
        "limit": 1
    })
    uid = data["queryUuid"]
    self._request("POST", f"/query_executions/{uid}:cancel", {})
    _, data, _ = self._request("GET", f"/query_executions/{uid}:status")
    self.assertEqual(data["status"], "SUCCESS")
    self.assertEqual(
        self.server.history_db.get_status(uid)["status"], "SUCCESS")

  def test_cancel_running_query_marks_cancelled(self):
    status, data, _ = self._request(
        "POST", "/execute_bigtrace_query_async", {
            "perfetto_sql": ("WITH RECURSIVE n(i) AS "
                             "(SELECT 0 UNION ALL SELECT i + 1 FROM n) "
                             "SELECT count(1) FROM n"),
            "limit": 1000000,
        })
    self.assertEqual(status, 200)
    uid = data["queryUuid"]
    status, _, _ = self._request("POST", f"/query_executions/{uid}:cancel", {})
    self.assertEqual(status, 200)
    status, data, _ = self._request("GET", f"/query_executions/{uid}:status")
    self.assertEqual(status, 200)
    self.assertEqual(data["status"], "CANCELLED")

  def test_sort_records_orders_numbers_then_strings_then_nulls(self):
    records = [{"val": v} for v in ("hello", 10, -5, None, 0, "apple")]
    result = sort_records(records, lambda r, f: r.get(f), [("val", False)])
    self.assertEqual([r["val"] for r in result],
                     [-5, 0, 10, "apple", "hello", None])

  def test_keep_alive_server_runs_repeated_queries(self):
    with running_server(
        trace_dir=TEST_DATA_DIR,
        manifest_path=MANIFEST_JSON,
        max_workers=2,
        keep_alive=True,
        pool_size=5) as server:
      url = base_url(server)
      # The second query runs against the TraceProcessors kept warm by the
      # first.
      for sql, column in (("SELECT count(1) AS c FROM slice;", "c"),
                          ("SELECT max(dur) AS max_dur FROM slice;",
                           "max_dur")):
        status, data, _ = request(
            url, "POST", "/execute_bigtrace_query", {
                "perfetto_sql": sql,
                "limit": 100,
                "trace_filters": [{
                    "field": "file_name",
                    "op": "glob",
                    "value": "*api24*"
                }],
                "trace_metadata_columns": ["file_name"],
            })
        self.assertEqual(status, 200)
        self.assertEqual(len(data["rows"]), 3)
        self.assertIn(column, [s["name"] for s in data["schema"]])


class BigtraceHistoryTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.tmp_dir = tmp.name
    self.db_file = os.path.join(self.tmp_dir, "history.db")

  def _open_db(self) -> BigtraceHistoryDb:
    db = BigtraceHistoryDb(self.db_file)
    self.addCleanup(db.close)
    return db

  def test_history_db_save_load_and_chunked_results(self):
    db = self._open_db()
    self.assertEqual(db.list_executions(), [])

    db.save_execution({
        "queryUuid": "uuid-1",
        "status": "SUCCESS",
        "startTime": "2026-09-28T12:00:00Z",
        "endTime": "2026-09-28T12:00:05Z",
        "processedRows": 100,
        "processedTraces": 2,
        "totalTraces": 2,
        "perfettoSql": "SELECT slice_id FROM slice",
        "limit": 1000,
        "materialized": True,
        "tableName": "bigtrace_test_table",
        "tableLink": "http://127.0.0.1:5001/trace_file?table=t",
        "schema": [{
            "name": "slice_id",
            "type": "INT64"
        }],
        "settings": [{
            "settingId": "trace_directory",
            "values": ["/path"],
            "category": "TRACE_ADDRESS"
        }],
        "_columns": ["slice_id", "file_name"],
        "_schema_map": {
            "slice_id": "INT64",
            "file_name": "STRING"
        },
    })
    rows = [[i, f"trace_{i % 10}.pftrace"] for i in range(3000)]
    db.save_results("uuid-1", rows, chunk_size=1000)
    db.save_execution({
        "queryUuid": "uuid-2",
        "status": "SUCCESS",
        "startTime": "2026-09-28T12:10:00Z",
        "perfettoSql": "SELECT count(*) FROM slice",
    })

    # Newest first.
    self.assertEqual([e["queryUuid"] for e in db.list_executions()],
                     ["uuid-2", "uuid-1"])
    loaded = db.get_execution("uuid-1")
    self.assertEqual(loaded["perfettoSql"], "SELECT slice_id FROM slice")
    self.assertEqual(loaded["tableName"], "bigtrace_test_table")
    self.assertEqual(loaded["_columns"], ["slice_id", "file_name"])
    self.assertEqual(db.get_results("uuid-1"), rows)

    self.assertTrue(db.delete_execution("uuid-1"))
    self.assertIsNone(db.get_execution("uuid-1"))
    self.assertEqual(db.get_results("uuid-1"), [])

  def test_history_db_cancels_in_progress_queries_on_reopen(self):
    db = self._open_db()
    db.save_execution({
        "queryUuid": "uuid-1",
        "status": "IN_PROGRESS",
        "startTime": "2026-09-28T12:10:00Z",
        "perfettoSql": "SELECT count(*) FROM slice",
    })
    db.close()

    status = self._open_db().get_status("uuid-1")
    self.assertEqual(status["status"], "CANCELLED")
    self.assertEqual(status["errorMessage"], "Interrupted by server restart")

  def test_server_restart_keeps_history_and_results(self):
    copy_trace(COLD_TRACE, self.tmp_dir, "trace.perfetto-trace")
    config = dict(trace_dir=self.tmp_dir, history_db=self.db_file)

    with running_server(**config) as server:
      url = base_url(server)
      status, data, _ = request(url, "POST", "/execute_bigtrace_query", {
          "perfetto_sql": "SELECT count(*) AS slice_count FROM slice;",
          "limit": 10
      })
      self.assertEqual(status, 200)
      uid = data["queryUuid"]
      self.assertIsNotNone(uid)
      self.assertGreater(len(data["rows"]), 0)
      self._assert_history(url, [(uid, "SUCCESS")])

    with running_server(**config) as server:
      url = base_url(server)
      self._assert_history(url, [(uid, "SUCCESS")])

      status, data, _ = request(url, "GET", f"/query_executions/{uid}")
      self.assertEqual(status, 200)
      self.assertEqual(data["queryUuid"], uid)
      self.assertIn("slice_count", data["perfettoSql"])

      # Results are loaded lazily from SQLite.
      status, data, _ = request(url, "POST",
                                f"/query_executions/{uid}:fetch_results", {
                                    "limit": 50,
                                    "offset": 0
                                })
      self.assertEqual(status, 200)
      self.assertEqual(data["queryUuid"], uid)
      self.assertGreater(len(data["rows"]), 0)

      status, _, _ = request(url, "DELETE", f"/query_executions/{uid}")
      self.assertEqual(status, 200)
      self._assert_history(url, [])

  def _assert_history(self, url, expected):
    status, data, _ = request(url, "GET", "/query_executions")
    self.assertEqual(status, 200)
    self.assertEqual([
        (e["queryUuid"], e["status"]) for e in data.get("queryExecutions", [])
    ], expected)


class TraceProcessorPoolTest(unittest.TestCase):

  @classmethod
  def setUpClass(cls):
    cls.tp_config = TraceProcessorConfig(bin_path=resolve_trace_processor_bin())
    cls.cold = trace_path(COLD_TRACE)
    cls.hot = trace_path(HOT_TRACE)
    cls.warm = trace_path(WARM_TRACE)

  def _keep_alive_pool(self) -> KeepAliveTraceProcessorPool:
    pool = KeepAliveTraceProcessorPool(tp_config=self.tp_config, max_size=2)
    self.addCleanup(pool.close)
    return pool

  def _idle(self, pool):
    with pool._lock:
      return dict(pool._idle)

  def test_ephemeral_pool_runs_queries(self):
    pool = EphemeralTraceProcessorPool(tp_config=self.tp_config)
    self.addCleanup(pool.close)
    tp = pool.acquire(self.cold)
    rows = list(tp.query("SELECT 42 AS val;"))
    self.assertEqual([r.val for r in rows], [42])
    pool.release(self.cold, tp)

  def test_keep_alive_pool_reuses_released_instance(self):
    pool = self._keep_alive_pool()
    first = pool.acquire(self.cold)
    self.assertFalse(pool.is_loaded(self.cold))
    self.assertEqual(list(first.query("SELECT 100 AS num;"))[0].num, 100)
    pool.release(self.cold, first)
    self.assertTrue(pool.is_loaded(self.cold))
    second = pool.acquire(self.cold)
    self.assertIs(first, second)
    pool.release(self.cold, second)

  def test_load_failure_reports_shell_error(self):
    pool = self._keep_alive_pool()
    with self.assertRaisesRegex(TraceProcessorException, "Unknown trace type"):
      pool.acquire(os.path.join(TEST_DATA_DIR, "..", "..", "OWNERS"))

  def test_keep_alive_pool_evicts_least_recently_used(self):
    pool = self._keep_alive_pool()
    for trace in (self.cold, self.hot, self.warm):
      pool.release(trace, pool.acquire(trace))
    self.assertEqual(list(self._idle(pool)), [self.hot, self.warm])

  def test_keep_alive_pool_drops_discarded_instance(self):
    pool = self._keep_alive_pool()
    pool.release(self.hot, pool.acquire(self.hot))
    self.assertIn(self.hot, self._idle(pool))
    pool.release(self.hot, pool.acquire(self.hot), discard=True)
    self.assertNotIn(self.hot, self._idle(pool))

  def test_keep_alive_pool_gives_concurrent_users_distinct_instances(self):
    pool = self._keep_alive_pool()
    tp_a = pool.acquire(self.warm)
    tp_b = pool.acquire(self.warm)
    self.assertIsNot(tp_a, tp_b)
    pool.release(self.warm, tp_a)
    # The second instance is closed rather than kept.
    pool.release(self.warm, tp_b)
    self.assertIs(self._idle(pool)[self.warm], tp_a)

  def test_load_trace_with_set_cancel_event_aborts(self):
    cancel_event = threading.Event()
    cancel_event.set()
    pool = EphemeralTraceProcessorPool(tp_config=self.tp_config)
    self.addCleanup(pool.close)
    with self.assertRaises(TraceProcessorException):
      pool.acquire(self.cold, cancel_event=cancel_event)


if __name__ == "__main__":
  unittest.main()
