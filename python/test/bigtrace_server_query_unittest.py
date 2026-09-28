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
"""Tests for result decoding, row limits and cancellation of running queries."""

import base64
import tempfile
import time
import unittest

from perfetto.bigtrace.server import (BigtraceQueryExecutor, TraceCatalog,
                                      resolve_trace_processor_bin)
from perfetto.bigtrace.server import query_result
from perfetto.trace_processor import (TraceProcessor, TraceProcessorConfig,
                                      TraceProcessorException)
from test.bigtrace_server_fixture import (COLD_TRACE, HOT_TRACE, WARM_TRACE,
                                          copy_trace, trace_path)

# Runs until killed.
ENDLESS_SQL = ("WITH RECURSIVE n(i) AS "
               "(SELECT 0 UNION ALL SELECT i + 1 FROM n) "
               "SELECT count(1) FROM n")


def wait_for(condition, timeout=10.0):
  deadline = time.monotonic() + timeout
  while not condition():
    if time.monotonic() > deadline:
      raise AssertionError("Timed out waiting for condition")
    time.sleep(0.02)


class QueryResultDecodingTest(unittest.TestCase):

  @classmethod
  def setUpClass(cls):
    cls.tp = TraceProcessor(
        trace=trace_path(COLD_TRACE),
        config=TraceProcessorConfig(bin_path=resolve_trace_processor_bin()))

  @classmethod
  def tearDownClass(cls):
    cls.tp.close()

  def _decode(self, sql):
    columns, batches = query_result.query(self.tp, sql)
    return columns, [row for decode in batches for row in decode()]

  def test_matches_trace_processor_query(self):
    for sql in ("SELECT * FROM slice", "SELECT * FROM thread",
                "SELECT * FROM slice WHERE 0"):
      with self.subTest(sql):
        expected = self.tp.query(sql)
        columns = list(expected.column_names)
        rows = [[getattr(r, c) for c in columns] for r in expected]
        self.assertEqual(self._decode(sql), (columns, rows))

  def test_decodes_every_cell_type(self):
    columns, rows = self._decode(
        "SELECT -1 AS neg, -9223372036854775808 AS min, "
        "9223372036854775807 AS max, 1.5 AS real, NULL AS null_value, "
        "'' AS empty, 'h\u00e9llo' AS text, x'00ff' AS blob")
    self.assertEqual(
        columns,
        ["neg", "min", "max", "real", "null_value", "empty", "text", "blob"])
    self.assertEqual(rows, [[
        -1, -2**63, 2**63 - 1, 1.5, None, "", "h\u00e9llo",
        base64.b64encode(b"\x00\xff").decode()
    ]])

  def test_query_error_raises(self):
    with self.assertRaises(TraceProcessorException):
      self._decode("SELECT * FROM no_such_table")


class QueryExecutionControlTest(unittest.TestCase):

  def setUp(self):
    tmp = tempfile.TemporaryDirectory()
    self.addCleanup(tmp.cleanup)
    for name in (COLD_TRACE, HOT_TRACE, WARM_TRACE):
      copy_trace(name, tmp.name, name)
    self.executor = BigtraceQueryExecutor(
        catalog=TraceCatalog(trace_dir=tmp.name),
        tp_config=TraceProcessorConfig(bin_path=resolve_trace_processor_bin()),
        max_workers=3,
        keep_alive=True)
    self.addCleanup(self.executor.close)

  def _run(self, sql, limit):
    return self.executor.execute({"perfetto_sql": sql, "limit": limit})

  def test_limit_applies_across_traces(self):
    result = self._run("SELECT * FROM slice", limit=7)
    self.assertEqual(result["totalFilteredRows"], 7)

    total = 0
    for name in (COLD_TRACE, HOT_TRACE, WARM_TRACE):
      with TraceProcessor(
          trace=trace_path(name),
          config=TraceProcessorConfig(
              bin_path=resolve_trace_processor_bin())) as tp:
        total += len(tp.query("SELECT id FROM slice"))
    result = self._run("SELECT * FROM slice", limit=10**7)
    self.assertEqual(result["totalFilteredRows"], total)

  def test_cancel_kills_running_trace_processors(self):
    uid = self.executor.execute({"perfetto_sql": ENDLESS_SQL},
                                is_async=True)["queryUuid"]
    ex = self.executor._executions[uid]
    wait_for(lambda: len(ex["_active"]) == 3)
    processes = [tp.subprocess for tp in list(ex["_active"])]

    self.executor.cancel_query(uid)

    for p in processes:
      p.wait(timeout=5)
    wait_for(lambda: not ex["_active"])
    self.assertEqual(self.executor.get_status(uid)["status"], "CANCELLED")
    self.assertFalse(ex.get("errorMessage"))
    # Killed instances are not reused; the next query loads fresh ones.
    self.assertEqual(self._run("SELECT 1 AS one", 10)["totalFilteredRows"], 3)

  def test_delete_kills_running_trace_processors(self):
    uid = self.executor.execute({"perfetto_sql": ENDLESS_SQL},
                                is_async=True)["queryUuid"]
    ex = self.executor._executions[uid]
    wait_for(lambda: len(ex["_active"]) == 3)
    processes = [tp.subprocess for tp in list(ex["_active"])]

    self.assertTrue(self.executor.delete_execution(uid))

    for p in processes:
      p.wait(timeout=5)


if __name__ == "__main__":
  unittest.main()
