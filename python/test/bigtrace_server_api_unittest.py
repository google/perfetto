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
"""HTTP API tests for the Bigtrace server."""

import time
import unittest

from test.bigtrace_server_fixture import BigtraceServerTestCase, request_raw

API24_FILTER = [{"field": "file_name", "op": "glob", "value": "*api24*"}]
TERMINAL_STATES = ("SUCCESS", "FAILED", "CANCELLED")


class BigtraceServerApiTest(BigtraceServerTestCase):

  def _metadata(self, **body):
    status, data, _ = self._request("POST", "/trace_metadata", body)
    self.assertEqual(status, 200)
    return data

  def _wait_until_done(self, query_uuid: str, timeout_s: float = 5) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
      status, data, _ = self._request("GET",
                                      f"/query_executions/{query_uuid}:status")
      self.assertEqual(status, 200)
      if data.get("status") in TERMINAL_STATES:
        return data["status"]
      time.sleep(0.1)
    self.fail(f"Query {query_uuid} did not finish within {timeout_s}s")

  def test_options_returns_cors_headers(self):
    status, _, headers = self._request("OPTIONS", "/v1/trace_metadata")
    self.assertEqual(status, 200)
    self.assertIn("Access-Control-Allow-Origin", headers)
    self.assertIn("Access-Control-Allow-Methods", headers)

  def test_head_returns_empty_response_with_cors(self):
    status, raw, headers = request_raw(self.base_url, "HEAD", "/")
    self.assertEqual(status, 200)
    self.assertEqual(raw, b"")
    self.assertIn("Access-Control-Allow-Origin", headers)
    self.assertEqual(headers.get("Content-Length"), "0")

  def test_cors_only_allows_trusted_origins(self):

    def cors_headers(origin):
      _, _, headers = request_raw(
          self.base_url, "GET", "/", headers={"Origin": origin})
      return (headers.get("Access-Control-Allow-Origin"),
              headers.get("Access-Control-Allow-Credentials"))

    # The UI fetches with credentials, so allowed origins need both headers.
    for origin in ("http://localhost:10000", "https://ui.perfetto.dev"):
      self.assertEqual(cors_headers(origin), (origin, "true"))
    self.assertEqual(cors_headers("https://evil.example.com"), (None, None))

  def test_execution_config_lists_settings(self):
    for path in ("/bigtrace_execution_config", "/v1/bigtrace_execution_config"):
      with self.subTest(path=path):
        status, data, _ = self._request("POST", path, {})
        self.assertEqual(status, 200)
        ids = {s["id"] for s in data["setting"]}
        self.assertLessEqual(
            {"trace_directory", "recursive", "trace_manifest", "trace_uuids"},
            ids)

  def test_metadata_schema_includes_manifest_columns(self):
    status, data, _ = self._request("POST", "/trace_metadata_schema", {})
    self.assertEqual(status, 200)
    names = {c["name"] for c in data["columns"]}
    self.assertLessEqual(
        {
            "link", "file_name", "file_size", "trace_uuid", "device_name",
            "experiment_id"
        }, names)

  def test_metadata_projects_requested_columns(self):
    columns = ["file_name", "device_name", "tag"]
    data = self._metadata(limit=10, offset=0, columns=columns)
    self.assertIn("totalFilteredRows", data)
    self.assertEqual(data["columnNames"], columns)
    self.assertGreaterEqual(len(data["rows"]), 7)
    self.assertEqual(len(data["rows"][0]["values"]), 3)

  def test_metadata_glob_filter(self):
    data = self._metadata(
        limit=10,
        filters=[{
            "field": "file_name",
            "op": "glob",
            "value": "*cold*"
        }])
    for row in data["rows"]:
      self.assertIn("cold", " ".join(str(v) for v in row["values"]))

  def test_metadata_equality_filter(self):
    data = self._metadata(
        limit=10,
        filters=[{
            "field": "device_name",
            "op": "=",
            "value": "Pixel 7"
        }])
    self.assertEqual(data["totalFilteredRows"], 3)

  def test_metadata_in_filter(self):
    data = self._metadata(
        limit=10,
        filters=[{
            "field": "device_name",
            "op": "in",
            "value": ["Pixel 6", "Pixel 8"]
        }])
    self.assertEqual(data["totalFilteredRows"], 4)

  def test_metadata_boolean_filter_accepts_lowercase_string(self):
    # The UI sends booleans as the string "true".
    data = self._metadata(
        limit=10,
        filters=[{
            "field": "is_treatment",
            "op": "=",
            "value": "true"
        }])
    self.assertGreater(data["totalFilteredRows"], 0)

  def test_metadata_order_by_desc(self):
    data = self._metadata(
        limit=10, order_by="file_size desc", columns=["file_name", "file_size"])
    sizes = [int(r["values"][1]) for r in data["rows"]]
    self.assertEqual(sizes, sorted(sizes, reverse=True))

  def test_metadata_experiment_filter(self):
    data = self._metadata(
        limit=10,
        experiment_filter={
            "experiment_id": 101,
            "control_id": 102,
            "is_treatment": True,
        })
    self.assertEqual(data["totalFilteredRows"], 2)

  def test_experiment_search_and_metadata(self):
    status, data, _ = self._request("POST", "/experiments",
                                    {"search_query": "PGO"})
    self.assertEqual(status, 200)
    self.assertEqual([e["experimentId"] for e in data["experiments"]], [101])

    status, data, _ = self._request("POST", "/experiment_metadata",
                                    {"experiment_id": 101})
    self.assertEqual(status, 200)
    self.assertEqual(data["experiment"]["experimentName"],
                     "Profile-Guided Optimization (PGO)")

  def test_trace_presets(self):
    status, data, _ = self._request("GET", "/trace_presets")
    self.assertEqual(status, 200)
    self.assertIn("preset:app_startup_latency",
                  [p["id"] for p in data["tracePresets"]])

  def test_check_table_exists(self):
    status, data, _ = self._request("POST", "/check_table_exists",
                                    {"table_name": "my_table"})
    self.assertEqual(status, 200)
    self.assertEqual(data["exists"], False)
    self.assertEqual(data["resolvedTableName"], "my_table")

  def test_link_opens_trace_in_perfetto_ui(self):
    data = self._metadata(limit=1, columns=["link"])
    link = data["rows"][0]["values"][0]
    prefix = "https://ui.perfetto.dev/#!/?url="
    self.assertTrue(link.startswith(prefix), link)
    trace_url = link[len(prefix):].split("&")[0]
    self.assertTrue(trace_url.startswith(self.base_url + "/trace/"), link)
    self.assertNotIn("?", trace_url)

    status, raw, headers = request_raw(
        self.base_url,
        "GET",
        trace_url[len(self.base_url):],
        headers={"Origin": "https://ui.perfetto.dev"})
    self.assertEqual(status, 200)
    self.assertGreater(len(raw), 1000)
    self.assertEqual(
        headers.get("Access-Control-Allow-Origin"), "https://ui.perfetto.dev")

  def test_unknown_trace_download_is_404(self):
    status, _, _ = self._request("GET", "/trace/no-such-uuid/x.pftrace")
    self.assertEqual(status, 404)

  def test_sync_query_returns_rows_with_metadata_columns(self):
    status, data, _ = self._request(
        "POST", "/execute_bigtrace_query", {
            "perfetto_sql": "SELECT count(1) AS slice_count FROM slice;",
            "limit": 100,
            "trace_filters": API24_FILTER,
            "trace_metadata_columns": ["file_name", "device_name"],
        })
    self.assertEqual(status, 200)
    self.assertIn("queryUuid", data)
    self.assertEqual(len(data["rows"]), 3)
    self.assertLessEqual({"slice_count", "file_name", "device_name"},
                         {s["name"] for s in data["schema"]})

  def test_sync_query_failure_returns_400(self):
    status, data, _ = self._request(
        "POST", "/execute_bigtrace_query", {
            "perfetto_sql": "SELECT * FROM definitely_non_existent_table;",
            "limit": 10,
        })
    self.assertEqual(status, 400)
    self.assertIn("detail", data)

  def test_async_query_polling_and_fetch_results(self):
    status, data, _ = self._request(
        "POST", "/execute_bigtrace_query_async", {
            "perfetto_sql":
                "SELECT name, dur FROM slice WHERE dur > 10000000 LIMIT 5;",
            "limit":
                50,
            "trace_filters":
                API24_FILTER,
            "trace_metadata_columns": ["file_name", "tag"],
        })
    self.assertEqual(status, 200)
    uid = data["queryUuid"]
    self.assertTrue(uid)
    self.assertEqual(self._wait_until_done(uid), "SUCCESS")

    status, data, _ = self._request("GET", f"/query_executions/{uid}")
    self.assertEqual(status, 200)
    self.assertEqual(data["status"], "SUCCESS")
    self.assertGreater(data["processedRows"], 0)

    status, data, _ = self._request(
        "POST", f"/query_executions/{uid}:fetch_results", {
            "limit": 10,
            "offset": 0,
            "order_by": "dur desc",
            "columns": ["name", "dur", "file_name"],
        })
    self.assertEqual(status, 200)
    self.assertIn("rows", data)
    self.assertLessEqual({"name", "dur", "tag"},
                         set(data["availableColumnNames"]))
    self.assertEqual([s["name"] for s in data["schema"][:2]], ["name", "dur"])
    durs = [int(r["values"][1]) for r in data["rows"] if r["values"][1]]
    self.assertEqual(durs, sorted(durs, reverse=True))

  def test_query_history_listing_and_deletion(self):
    _, data, _ = self._request("POST", "/execute_bigtrace_query", {
        "perfetto_sql": "SELECT 1 AS val;",
        "limit": 1
    })
    uid = data["queryUuid"]

    status, data, _ = self._request("GET", "/query_executions")
    self.assertEqual(status, 200)
    self.assertIn(uid, [q["queryUuid"] for q in data["queryExecutions"]])

    status, _, _ = self._request("DELETE",
                                 f"/query_executions/{uid}?drop_table=true")
    self.assertEqual(status, 200)

    status, data, _ = self._request("GET", f"/query_executions/{uid}")
    self.assertEqual(status, 404)
    self.assertIn("detail", data)

  def test_query_listing_omits_unset_fields(self):
    status, _, _ = self._request("POST", "/execute_bigtrace_query", {
        "perfetto_sql": "SELECT 1 AS val;",
        "limit": 10
    })
    self.assertEqual(status, 200)
    status, data, _ = self._request("GET", "/query_executions")
    self.assertEqual(status, 200)
    latest = data["queryExecutions"][0]
    self.assertIn("materialized", latest)
    self.assertIn("schema", latest)
    self.assertNotIn("error", latest)
    self.assertNotIn("tableLink", latest)


if __name__ == "__main__":
  unittest.main()
