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
"""Manifest generation and trace catalog tests for the Bigtrace server."""

import os
import tempfile
import unittest

from perfetto.bigtrace.server import (
    BigtraceManifestGenerator,
    BigtraceQueryExecutor,
    TraceCatalog,
)
from perfetto.trace_processor import TraceProcessorConfig
from test.bigtrace_server_fixture import (
    BigtraceServerTestCase,
    COLD_TRACE,
    HOT_TRACE,
    MANIFEST_CSV,
    MANIFEST_JSON,
    TEST_DATA_DIR,
    WARM_TRACE,
    base_url,
    copy_trace,
    request,
    running_server,
)


def write_file(path: str, content: bytes) -> None:
  with open(path, "wb") as f:
    f.write(content)


class BigtraceServerManifestTest(BigtraceServerTestCase):

  def setUp(self):
    super().setUp()
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    self.tmp_dir = tmp.name

  def _generator(self, **kwargs) -> BigtraceManifestGenerator:
    kwargs.setdefault("max_workers", 2)
    return BigtraceManifestGenerator(
        trace_dir=self.tmp_dir, bin_path=self.bin_path, **kwargs)

  def test_generator_keeps_traces_and_discards_non_traces(self):
    d = self.tmp_dir
    copy_trace(COLD_TRACE, d, "trace1.perfetto-trace")
    copy_trace("android_job_scheduler.perfetto-trace", d,
               "trace2.perfetto-trace")
    copy_trace(HOT_TRACE, d, "trace3.customext")
    copy_trace(WARM_TRACE, d, "trace4_no_ext")
    os.makedirs(os.path.join(d, "nested"))
    copy_trace("track_event_with_description.perfetto-trace",
               os.path.join(d, "nested"), "trace5.perfetto-trace")
    write_file(os.path.join(d, "notes.txt"), b"Plain text, not a trace.")
    write_file(os.path.join(d, "empty.bin"), b"")
    write_file(
        os.path.join(d, "corrupt.perfetto-trace"), b"\xde\xad\xbe\xef" * 16)

    manifest_path = os.path.join(d, "bigtrace_manifest.json")
    manifest = self._generator(
        recursive=True, max_workers=4).generate(output_path=manifest_path)

    self.assertTrue(os.path.isfile(manifest_path))
    self.assertEqual(len(manifest["traces"]), 5)
    schema = {c["name"]: c for c in manifest["schema"]}
    self.assertLessEqual(
        {
            "file_name", "duration_ms", "slice_count", "process_count",
            "thread_count"
        }, set(schema))
    self.assertEqual(schema["duration_ms"].get("displayName"), "Duration (ms)")
    self.assertEqual(schema["duration_ms"].get("type"), "FLOAT64")
    self.assertEqual(schema["slice_count"].get("displayName"), "Slice Count")
    self.assertEqual(schema["slice_count"].get("type"), "INT64")

    with running_server(
        trace_dir=d, manifest_path=manifest_path, max_workers=2) as server:
      url = base_url(server)
      status, data, _ = request(url, "POST", "/trace_metadata_schema", {})
      self.assertEqual(status, 200)
      self.assertLessEqual({"duration_ms", "slice_count"},
                           {c["name"] for c in data["columns"]})

      status, data, _ = request(
          url, "POST", "/trace_metadata",
          {"columns": ["file_name", "duration_ms", "slice_count"]})
      self.assertEqual(status, 200)
      self.assertEqual(data["totalFilteredRows"], 5)
      self.assertEqual(len(data["rows"]), 5)

      status, data, _ = request(
          url, "POST", "/execute_bigtrace_query", {
              "perfetto_sql": "SELECT count(1) AS total_slices FROM slice;",
              "limit": 10
          })
      self.assertEqual(status, 200)
      self.assertGreater(len(data["rows"]), 0)

  def test_generator_custom_metadata_query(self):
    copy_trace(COLD_TRACE, self.tmp_dir, "cold.perfetto-trace")
    manifest = self._generator(
        recursive=False,
        metadata_query=("SELECT count(1) AS ui_slices_count FROM slice "
                        "WHERE name LIKE '%Choreographer%';"),
    ).generate(output_path=os.path.join(self.tmp_dir, "manifest.json"))
    self.assertEqual(len(manifest["traces"]), 1)
    self.assertGreater(manifest["traces"][0]["ui_slices_count"], 0)
    self.assertIn("ui_slices_count", [c["name"] for c in manifest["schema"]])

  def test_generator_incremental_refresh_reuses_and_prunes(self):
    t1 = copy_trace(COLD_TRACE, self.tmp_dir, "t1.perfetto-trace")
    copy_trace(WARM_TRACE, self.tmp_dir, "t2.perfetto-trace")
    manifest_path = os.path.join(self.tmp_dir, "manifest.json")
    gen = self._generator()

    def uuids_by_name(manifest):
      return {t["file_name"]: t["trace_uuid"] for t in manifest["traces"]}

    first = uuids_by_name(gen.generate(output_path=manifest_path))
    self.assertEqual(len(first), 2)

    copy_trace("track_event_with_description.perfetto-trace", self.tmp_dir,
               "t3.perfetto-trace")
    second = uuids_by_name(gen.generate(output_path=manifest_path))
    self.assertEqual(len(second), 3)
    self.assertEqual(second["t1.perfetto-trace"], first["t1.perfetto-trace"])

    os.remove(t1)
    third = uuids_by_name(gen.generate(output_path=manifest_path))
    self.assertEqual(set(third), {"t2.perfetto-trace", "t3.perfetto-trace"})

  def test_refresh_endpoint_and_restart_pick_up_regenerated_manifest(self):
    copy_trace(COLD_TRACE, self.tmp_dir, "trace_a.perfetto-trace")
    manifest_path = os.path.join(self.tmp_dir, "bigtrace_manifest.json")
    gen = self._generator()
    gen.generate(output_path=manifest_path)
    config = dict(trace_dir=self.tmp_dir, manifest_path=manifest_path)

    with running_server(**config) as server:
      self.assertEqual(len(server.catalog.get_traces()), 1)
      copy_trace(WARM_TRACE, self.tmp_dir, "trace_b.perfetto-trace")
      gen.generate(output_path=manifest_path)

      status, data, _ = request(base_url(server), "POST", "/refresh", {})
      self.assertEqual(status, 200)
      self.assertEqual(data["status"], "ok")
      self.assertEqual(data["traceCount"], 2)
      self.assertEqual(len(server.catalog.get_traces()), 2)

    with running_server(**config) as server:
      self.assertEqual(len(server.catalog.get_traces()), 2)

  def test_catalog_is_configured_with(self):
    catalog = TraceCatalog(manifest_path=MANIFEST_JSON)
    self.assertTrue(catalog.is_configured_with(manifest_path=MANIFEST_JSON))
    self.assertFalse(catalog.is_configured_with(trace_dir="/tmp/different_dir"))

  def test_catalog_recursive_scanning(self):
    d = self.tmp_dir
    os.makedirs(os.path.join(d, "a"))
    os.makedirs(os.path.join(d, "b"))
    copy_trace(COLD_TRACE, d, "root.perfetto-trace")
    copy_trace(HOT_TRACE, os.path.join(d, "a"), "a.perfetto-trace")
    copy_trace(WARM_TRACE, os.path.join(d, "b"), "b.perfetto-trace")

    flat = TraceCatalog(trace_dir=d, recursive=False).get_traces()
    self.assertEqual([t.file_name for t in flat], ["root.perfetto-trace"])
    self.assertEqual(
        len(TraceCatalog(trace_dir=d, recursive=True).get_traces()), 3)

    status, data, _ = self._request(
        "POST", "/trace_metadata", {
            "settings": [
                {
                    "setting_id": "trace_directory",
                    "values": [d],
                    "category": "TRACE_ADDRESS",
                },
                {
                    "setting_id": "recursive",
                    "values": ["true"],
                    "category": "TRACE_ADDRESS",
                },
            ],
            "limit": 10,
            "columns": ["file_name"],
        })
    self.assertEqual(status, 200)
    self.assertEqual(data["totalFilteredRows"], 3)

  def test_catalog_loads_csv_manifest(self):
    traces = TraceCatalog(
        trace_dir=TEST_DATA_DIR, manifest_path=MANIFEST_CSV).get_traces()
    self.assertEqual(len(traces), 7)
    first = traces[0]
    self.assertEqual(first.get_value("device_name"), "Pixel 6")
    self.assertEqual(first.get_value("build_id"), "NRD90M.220624.014")
    self.assertEqual(first.get_value("tag"), "cold")

  def test_query_skips_non_trace_files_in_directory(self):
    d = self.tmp_dir
    copy_trace(COLD_TRACE, d, "trace_no_extension")
    copy_trace(HOT_TRACE, d, "trace_custom.xyz123")
    write_file(os.path.join(d, "notes.txt"), b"Plain text, not a trace.")
    write_file(os.path.join(d, "random.bin"), b"\x00\x01\x02\x03\x04\x05")

    # Directory scans list every file regardless of extension.
    catalog = TraceCatalog(trace_dir=d, recursive=False)
    self.assertEqual({t.file_name for t in catalog.get_traces()}, {
        "trace_no_extension", "trace_custom.xyz123", "notes.txt", "random.bin"
    })

    executor = BigtraceQueryExecutor(
        catalog=catalog,
        tp_config=TraceProcessorConfig(bin_path=self.bin_path),
        max_workers=2,
    )
    self.addCleanup(executor.close)
    result = executor.execute({
        "perfetto_sql": "SELECT count(1) AS slice_count FROM slice;",
        "limit": 100,
        "trace_metadata_columns": ["file_name"],
    })
    self.assertEqual({r["values"][1] for r in result["rows"]},
                     {"trace_no_extension", "trace_custom.xyz123"})
    self.assertEqual(len(result["rows"]), 2)

  def test_empty_setting_overrides_keep_manifest(self):
    empty = [{
        "setting_id": s,
        "values": [""]
    } for s in ("trace_directory", "trace_manifest", "trace_uuids")]
    status, data, _ = self._request("POST", "/trace_metadata_schema",
                                    {"settings": empty})
    self.assertEqual(status, 200)
    self.assertIn("device_name", [c["name"] for c in data["columns"]])
    self.assertEqual(self.server.catalog.manifest_path, MANIFEST_JSON)


if __name__ == "__main__":
  unittest.main()
