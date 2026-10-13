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
"""Query execution, DuckDB persistence, and pool tests for the Bigtrace server."""

import concurrent.futures
import os
import tempfile
import threading
import unittest

from perfetto.bigtrace.server import (
    DuckDBEngine,
    TraceProcessorPool,
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
    ENDLESS_SQL,
    MANIFEST_JSON,
    TEST_DATA_DIR,
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
    self.assertEqual(self.server.duckdb.get_status(uid)["status"], "SUCCESS")

  def test_cancel_running_query_marks_cancelled(self):
    status, data, _ = self._request("POST", "/execute_bigtrace_query_async", {
        "perfetto_sql": ENDLESS_SQL,
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

  def test_duckdb_materialization_and_multi_table_joins(self):
    with tempfile.TemporaryDirectory() as parquet_dir:
      with running_server(
          trace_dir=TEST_DATA_DIR,
          manifest_path=MANIFEST_JSON,
          max_workers=2,
          parquet_dir=parquet_dir) as server:
        url = base_url(server)
        # Run 1 -> automatically materialized into result_1 with _trace_uuid.
        status, q1, _ = request(
            url, "POST", "/execute_bigtrace_query", {
                "perfetto_sql": "SELECT count(1) AS slice_cnt FROM slice;",
                "limit": 100,
                "trace_filters": [{
                    "field": "file_name",
                    "op": "glob",
                    "value": "*api24*"
                }],
                "trace_metadata_columns": ["file_name"],
            })
        self.assertEqual(status, 200)
        self.assertEqual(len(q1["rows"]), 3)
        self.assertEqual(q1["tableName"], "result_1")
        schema_names = [s["name"] for s in q1["schema"]]
        self.assertIn("_trace_uuid", schema_names)
        self.assertIn("slice_cnt", schema_names)

        # Run 2 -> custom alternate table name "max_durs".
        status, q2, _ = request(
            url, "POST", "/execute_bigtrace_query", {
                "perfetto_sql":
                    "SELECT max(dur) AS max_dur FROM slice;",
                "limit":
                    100,
                "table_name":
                    "max_durs",
                "trace_filters": [{
                    "field": "file_name",
                    "op": "glob",
                    "value": "*api24*"
                }],
            })
        self.assertEqual(status, 200)
        self.assertEqual(q2["tableName"], "max_durs")

        # Run 3 -> Single-pass multi-query execution amortizing trace load.
        status, batch_res, _ = request(
            url, "POST", "/execute_bigtrace_query", {
                "queries": [
                    {
                        "perfetto_sql":
                            "SELECT min(dur) AS min_dur FROM slice;",
                        "table_name":
                            "min_durs"
                    },
                    {
                        "perfetto_sql":
                            "SELECT sum(dur) AS sum_dur FROM slice;",
                        "table_name":
                            "sum_durs"
                    },
                ],
                "limit":
                    100,
                "trace_filters": [{
                    "field": "file_name",
                    "op": "glob",
                    "value": "*api24*"
                }],
            })
        self.assertEqual(status, 200, batch_res)
        self.assertEqual(len(batch_res["executions"]), 2)
        self.assertEqual(
            [e["tableName"] for e in batch_res["executions"]],
            ["min_durs", "sum_durs"],
        )

        # List DuckDB tables.
        status, tables_resp, _ = request(url, "GET", "/duckdb/tables")
        self.assertEqual(status, 200)
        table_names = {t["name"] for t in tables_resp["tables"]}
        self.assertLessEqual(
            {"result_1", "max_durs", "min_durs", "sum_durs", "trace_metadata"},
            table_names,
        )

        # Join across result_1, max_durs, min_durs, sum_durs, and trace_metadata.
        status, joined, _ = request(
            url, "POST", "/execute_bigtrace_query", {
                "engine":
                    "duckdb",
                "perfetto_sql":
                    """
                    SELECT
                      r._trace_uuid,
                      r.slice_cnt,
                      m.max_dur,
                      s.sum_dur,
                      t.device_name
                    FROM result_1 r
                    JOIN max_durs m USING (_trace_uuid)
                    JOIN sum_durs s USING (_trace_uuid)
                    JOIN trace_metadata t USING (_trace_uuid)
                    ORDER BY r.slice_cnt DESC
                """,
                "limit":
                    100,
            })
        self.assertEqual(status, 200, joined)
        self.assertEqual(len(joined["rows"]), 3)
        self.assertEqual(
            [s["name"] for s in joined["schema"]],
            ["_trace_uuid", "slice_cnt", "max_dur", "sum_dur", "device_name"],
        )

        # PerfettoSQL Next DuckDB extension operators work in DuckDB mode.
        status, ext_res, _ = request(
            url, "POST", "/execute_bigtrace_query", {
                "engine":
                    "duckdb",
                "perfetto_sql":
                    """
                    SELECT ts, dur, count FROM pf_interval_flatten_sum(
                      '(SELECT 1::BIGINT AS id, 0::BIGINT AS ts, 10::BIGINT AS dur UNION ALL SELECT 2::BIGINT AS id, 5::BIGINT AS ts, 10::BIGINT AS dur)'
                    ) ORDER BY ts
                """,
                "limit":
                    100,
            })
        self.assertEqual(status, 200, ext_res)
        self.assertEqual(len(ext_res["rows"]), 3)

      # Parquet files were exported and are reloaded on a fresh server start.
      self.assertTrue(
          os.path.isfile(os.path.join(parquet_dir, "result_1.parquet")))
      self.assertTrue(
          os.path.isfile(os.path.join(parquet_dir, "max_durs.parquet")))
      self.assertTrue(
          os.path.isfile(os.path.join(parquet_dir, "sum_durs.parquet")))
      with running_server(
          trace_dir=TEST_DATA_DIR,
          manifest_path=MANIFEST_JSON,
          parquet_dir=parquet_dir) as server2:
        url2 = base_url(server2)
        status, reloaded, _ = request(
            url2, "POST", "/execute_bigtrace_query", {
                "engine":
                    "duckdb",
                "perfetto_sql":
                    "SELECT count(*) AS n FROM result_1 JOIN max_durs USING (_trace_uuid) JOIN sum_durs USING (_trace_uuid)",
                "limit":
                    10,
            })
        self.assertEqual(status, 200, reloaded)
        self.assertEqual(reloaded["rows"][0]["values"], ["3"])


class DuckdbHistoryPersistenceTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.tmp_dir = tmp.name
    self.duckdb_file = os.path.join(self.tmp_dir, "bigtrace.duckdb")

  def _open_engine(self) -> DuckDBEngine:
    eng = DuckDBEngine(db_path=self.duckdb_file)
    self.addCleanup(eng.close)
    return eng

  def test_duckdb_save_load_and_cancel_in_progress_on_reopen(self):
    eng = self._open_engine()
    self.assertEqual(eng.list_executions(), [])
    eng.save_execution({
        "queryUuid": "uuid-1",
        "status": "SUCCESS",
        "startTime": "2026-09-28T12:00:00Z",
        "perfettoSql": "SELECT slice_id FROM slice",
        "tableName": "bigtrace_test_table",
        "_columns": ["slice_id", "file_name"],
    })
    eng.save_execution({
        "queryUuid": "uuid-2",
        "status": "IN_PROGRESS",
        "startTime": "2026-09-28T12:10:00Z",
        "perfettoSql": "SELECT count(*) FROM slice",
    })
    self.assertEqual([e["queryUuid"] for e in eng.list_executions()],
                     ["uuid-2", "uuid-1"])
    eng.close()

    eng2 = self._open_engine()
    status = eng2.get_status("uuid-2")
    self.assertEqual(status["status"], "CANCELLED")
    self.assertEqual(status["errorMessage"], "Interrupted by server restart")
    self.assertTrue(eng2.delete_execution_record("uuid-1"))
    self.assertIsNone(eng2.get_execution("uuid-1"))

  def test_server_restart_with_duckdb_path_keeps_history_and_results(self):
    copy_trace(COLD_TRACE, self.tmp_dir, "trace.perfetto-trace")
    config = dict(trace_dir=self.tmp_dir, duckdb_path=self.duckdb_file)

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

  def test_release_closes_trace_processor(self):
    pool = TraceProcessorPool(tp_config=self.tp_config)
    self.addCleanup(pool.close)
    tp = pool.acquire(self.cold)
    self.assertIsNotNone(tp.subprocess)
    pool.release(tp)
    self.assertIsNone(tp.subprocess)

  def test_load_failure_reports_shell_error(self):
    pool = TraceProcessorPool(tp_config=self.tp_config)
    self.addCleanup(pool.close)
    with self.assertRaisesRegex(TraceProcessorException, "Unknown trace type"):
      pool.acquire(os.path.join(TEST_DATA_DIR, "..", "..", "OWNERS"))

  def test_load_trace_with_set_cancel_event_aborts(self):
    cancel_event = threading.Event()
    cancel_event.set()
    pool = TraceProcessorPool(tp_config=self.tp_config)
    self.addCleanup(pool.close)
    with self.assertRaises(TraceProcessorException):
      pool.acquire(self.cold, cancel_event=cancel_event)


if __name__ == "__main__":
  unittest.main()
