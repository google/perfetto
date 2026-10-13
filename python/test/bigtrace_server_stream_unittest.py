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
"""Tests streaming ingest of trace processor results into DuckDB."""

import os
import socket
import tempfile
import threading
import time
import unittest

from perfetto.bigtrace.server import stream_ingest
from perfetto.bigtrace.server.duckdb_engine import DuckDBEngine
from perfetto.bigtrace.server.stream_ingest import StreamIngest
from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig
from test.bigtrace_server_fixture import (
    BigtraceServerTestCase,
    COLD_TRACE,
    resolve_trace_processor_bin,
    trace_path,
    wait_for,
)

_COLD_ONLY = [{"field": "file_name", "op": "glob", "value": "*startup_cold*"}]
_SLICE_SQL = "SELECT id, ts, dur, name, depth FROM slice"


def _varint(v):
  out = bytearray()
  while v >= 0x80:
    out.append((v & 0x7F) | 0x80)
    v >>= 7
  out.append(v)
  return bytes(out)


def _field(num, payload):
  return _varint((num << 3) | 2) + _varint(len(payload)) + payload


def _result(columns, rows):
  """Encodes a QueryResult; rows hold ints, floats, strings or None."""
  cells, varints, floats, strings = bytearray(), bytearray(), bytearray(), b""
  for row in rows:
    for v in row:
      if v is None:
        cells.append(1)
      elif isinstance(v, int):
        cells.append(2)
        varints += _varint(v & ((1 << 64) - 1))
      elif isinstance(v, float):
        cells.append(3)
        floats += __import__("struct").pack("<d", v)
      else:
        cells.append(4)
        strings += v.encode() + b"\0"
  batch = _field(1, bytes(cells)) + _field(2, bytes(varints))
  if floats:
    batch += _field(3, bytes(floats))
  if strings:
    batch += _field(5, strings)
  out = b"".join(_field(1, c.encode()) for c in columns)
  return out + _field(3, batch)


class _FakeTp:
  """Serves one canned HTTP reply per connection on a local port."""

  def __init__(self, reply: bytes, delay_after: int = -1):
    self.reply = reply
    self.delay_after = delay_after
    self.release = threading.Event()
    self.sock = socket.socket()
    self.sock.bind(("127.0.0.1", 0))
    self.sock.listen(8)
    self.port = self.sock.getsockname()[1]
    self.thread = threading.Thread(target=self._serve, daemon=True)
    self.thread.start()

  def _serve(self):
    while True:
      try:
        conn, _ = self.sock.accept()
      except OSError:
        return
      with conn:
        data = b""
        while b"\r\n\r\n" not in data:
          chunk = conn.recv(65536)
          if not chunk:
            break
          data += chunk
        try:
          if self.delay_after >= 0:
            conn.sendall(self.reply[:self.delay_after])
            self.release.wait(30)
            conn.sendall(self.reply[self.delay_after:])
          else:
            conn.sendall(self.reply)
        except OSError:
          pass

  def close(self):
    self.release.set()
    self.sock.close()


def _chunked(body: bytes, pieces: int = 3) -> bytes:
  out = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
  step = max(1, len(body) // pieces)
  for i in range(0, len(body), step):
    part = body[i:i + step]
    out += b"%x\r\n" % len(part) + part + b"\r\n"
  return out + b"0\r\n\r\n"


class StreamIngestTest(unittest.TestCase):
  """Drives the extension's stream API directly."""

  @classmethod
  def setUpClass(cls):
    cls.tmp = tempfile.TemporaryDirectory()
    cls.engine = DuckDBEngine(os.path.join(cls.tmp.name, "t.duckdb"))
    if cls.engine.stream_api is None:
      raise unittest.SkipTest("perfetto DuckDB extension unavailable")
    cls.api = cls.engine.stream_api

  @classmethod
  def tearDownClass(cls):
    cls.engine.close()
    cls.tmp.cleanup()

  def _ingest(self, table, columns, types, sql="SELECT 1", **kw):

    def on_schema(cols, typs):
      schema = dict(zip(cols, typs))
      schema["_trace_uuid"] = "STRING"
      self.engine.create_streaming_table(
          table, list(cols) + ["_trace_uuid"], schema, uuid_enum=False)
      return lambda handle: self.engine.insert_from_stream(
          table, handle, cols, typs, ["t0", "t1", "t2", "t3"], False, None, 0)

    return StreamIngest(self.api, sql, on_schema, columns=columns, **kw)

  def _rows(self, sql):
    return self.engine._conn.execute(sql).fetchall()

  def test_streams_fake_trace_processor_reply(self):
    body = _result(["a", "b", "c"], [(1, "x", 1.5), (None, "y", None),
                                     (-3, None, 2.0)])
    tp = _FakeTp(_chunked(body, pieces=7))
    try:
      ingest = self._ingest("fake1", ["a", "b", "c"],
                            ["INT64", "STRING", "DOUBLE"])
      res = ingest.feed(f"tcp:127.0.0.1:{tp.port}", 2)
      self.assertEqual(res.status, stream_ingest.FEED_OK, res.message)
      self.assertEqual(res.rows, 3)
      self.assertEqual(ingest.finish(), [])
    finally:
      tp.close()
    self.assertEqual(
        sorted(self._rows("SELECT a, b, c, _trace_uuid FROM fake1"), key=str),
        [(-3, None, 2.0, "t2"), (1, "x", 1.5, "t2"), (None, "y", None, "t2")])

  def test_reports_sql_errors_and_column_mismatches(self):
    err = _field(1, b"a") + _field(2, b"no such table: foo")
    other = _result(["z"], [(1,)])
    tp_err, tp_cols = _FakeTp(_chunked(err)), _FakeTp(_chunked(other))
    try:
      ingest = self._ingest("fake2", ["a"], ["INT64"])
      res = ingest.feed(f"tcp:127.0.0.1:{tp_err.port}", 0)
      self.assertEqual(res.status, stream_ingest.FEED_SQL_ERROR)
      self.assertIn("no such table", res.message)
      res = ingest.feed(f"tcp:127.0.0.1:{tp_cols.port}", 0)
      self.assertEqual(res.status, stream_ingest.FEED_COLUMN_MISMATCH)
      ingest.finish()
    finally:
      tp_err.close()
      tp_cols.close()
    self.assertEqual(self._rows("SELECT count(*) FROM fake2"), [(0,)])

  def test_truncated_reply_and_dead_trace_processor_are_io_errors(self):
    body = _chunked(_result(["a"], [(i,) for i in range(1000)]))
    tp = _FakeTp(body[:len(body) // 2])  # Closes mid-result.
    dead = socket.socket()
    dead.bind(("127.0.0.1", 0))
    dead_port = dead.getsockname()[1]
    dead.close()
    try:
      ingest = self._ingest("fake3", ["a"], ["INT64"])
      res = ingest.feed(f"tcp:127.0.0.1:{tp.port}", 0)
      self.assertEqual(res.status, stream_ingest.FEED_IO_ERROR)
      res = ingest.feed(f"tcp:127.0.0.1:{dead_port}", 0)
      self.assertEqual(res.status, stream_ingest.FEED_IO_ERROR)
      self.assertIn("connect", res.message)
      ingest.finish()
    finally:
      tp.close()

  def test_cancel_unblocks_stalled_feeds_promptly(self):
    body = _chunked(_result(["a"], [(i,) for i in range(10)]))
    tp = _FakeTp(body, delay_after=len(body) // 2)  # Stalls mid-result.
    try:
      ingest = self._ingest("fake4", ["a"], ["INT64"])
      out = []
      t = threading.Thread(
          target=lambda: out.append(ingest.feed(f"tcp:127.0.0.1:{tp.port}", 0)))
      t.start()
      time.sleep(0.3)
      start = time.monotonic()
      ingest.cancel()
      t.join(5)
      self.assertFalse(t.is_alive())
      self.assertLess(time.monotonic() - start, 2)
      self.assertEqual(out[0].status, stream_ingest.FEED_CANCELLED)
      ingest.finish()
    finally:
      tp.close()

  def test_full_queue_backpressure_and_cancel(self):
    # One chunk of queue and no consumer: a big result must block, not grow.
    handle = self.api.open("SELECT 1", ["a"], queue_chunks=1)
    rows = [(i,) for i in range(50000)]
    tp = _FakeTp(_chunked(_result(["a"], rows), pieces=50))
    out = []
    try:
      t = threading.Thread(target=lambda: out.append(
          self.api.feed(handle, f"tcp:127.0.0.1:{tp.port}", 0)))
      t.start()
      time.sleep(0.5)
      self.assertTrue(t.is_alive())
      self.api.cancel(handle)
      t.join(5)
      self.assertFalse(t.is_alive())
      self.assertEqual(out[0].status, stream_ingest.FEED_CANCELLED)
    finally:
      self.api.free(handle)
      tp.close()

  def test_segments_rotate_and_spool_files_are_consumed(self):
    ingest = self._ingest("fake5", ["a"], ["INT64"], segment_traces=2)
    tps = []
    try:
      for i in range(5):
        tp = _FakeTp(_chunked(_result(["a"], [(i,), (i * 10,)])))
        tps.append(tp)
        res = ingest.feed(f"tcp:127.0.0.1:{tp.port}", i % 4)
        self.assertEqual(res.status, stream_ingest.FEED_OK, res.message)
        self.assertEqual(res.rows, 2)
        if i == 0:
          wait_for(lambda: ingest.started, 2.0)
      self.assertEqual(ingest.finish(), [])
      self.assertGreaterEqual(len(ingest._segments), 3)
      self.assertEqual(
          self._rows("SELECT count(*), sum(a) FROM fake5"), [(10, 110)])
    finally:
      for tp in tps:
        tp.close()

  def test_parallel_inserts_drain_one_stream(self):
    ingest = self._ingest("fake7", ["a"], ["INT64"])
    tps = []
    try:
      for i in range(20):
        tp = _FakeTp(_chunked(_result(["a"], [(j,) for j in range(5000)])))
        tps.append(tp)
        self.assertTrue(ingest.feed(f"tcp:127.0.0.1:{tp.port}", i % 4).ok)
      self.assertEqual(ingest.finish(), [])
      self.assertEqual(self._rows("SELECT count(*) FROM fake7"), [(100000,)])
      self.assertEqual(
          self._rows("SELECT DISTINCT _trace_uuid FROM fake7 ORDER BY 1"),
          [("t0",), ("t1",), ("t2",), ("t3",)])
    finally:
      for tp in tps:
        tp.close()

  def test_cell_types_type_unknown_columns(self):
    body = _result(["a", "b", "c"], [(1, 1.5, "s"), (2, 3, None)])
    tp = _FakeTp(_chunked(body))
    try:
      ingest = self._ingest("fake6", ["a", "b", "c"],
                            ["STRING", "STRING", "STRING"])
      self.assertTrue(ingest.feed(f"tcp:127.0.0.1:{tp.port}", 0).ok)
      ingest.finish()
    finally:
      tp.close()
    types = [stream_ingest.type_from_cell_types(m) for m in ingest.cell_types]
    self.assertEqual(types, ["INT64", "DOUBLE", "STRING"])
    self.assertEqual(
        sorted(self._rows("SELECT a, b FROM fake6")), [(1, 1.5), (2, 3.0)])

  def test_matches_real_trace_processor(self):
    tp = TraceProcessor(
        trace=trace_path(COLD_TRACE),
        config=TraceProcessorConfig(bin_path=resolve_trace_processor_bin()))
    try:
      exp_rows = [
          (r.id, r.ts, r.dur, r.name, r.depth) for r in tp.query(_SLICE_SQL)
      ]
      ingest = self._ingest(
          "real1", ["id", "ts", "dur", "name", "depth"],
          ["INT64", "INT64", "INT64", "STRING", "INT64"],
          sql=_SLICE_SQL)
      conn = tp.http.conn
      res = ingest.feed(f"tcp:{conn.host}:{conn.port}", 1)
      self.assertEqual(res.status, stream_ingest.FEED_OK, res.message)
      self.assertEqual(ingest.finish(), [])
    finally:
      tp.close()
    self.assertGreater(len(exp_rows), 1000)
    self.assertEqual(res.rows, len(exp_rows))
    got = self._rows("SELECT id, ts, dur, name, depth FROM real1 ORDER BY id")
    self.assertEqual(got, sorted(exp_rows))


class StreamingQueryTest(BigtraceServerTestCase):
  """Unlimited (limit=0) trace queries go through streaming ingest."""

  def _run(self, sql, limit=0, filters=None):
    status, data, _ = self._request(
        "POST", "/execute_bigtrace_query", {
            "perfetto_sql": sql,
            "limit": limit,
            **({
                "trace_filters": filters
            } if filters else {})
        })
    self.assertEqual(status, 200, data)
    return data

  def _status(self, uid):
    return self._request("GET", f"/query_executions/{uid}")[1]

  def _count(self, table):
    status, res, _ = self._request("POST", "/execute_bigtrace_query", {
        "engine": "duckdb",
        "perfetto_sql": f"SELECT count(*) AS n FROM {table}",
    })
    self.assertEqual(status, 200, res)
    return int(res["rows"][0]["values"][0])

  def test_unlimited_matches_limited_rows(self):
    if self.server.duckdb.stream_api is None:
      self.skipTest("streaming unavailable")
    sql = "SELECT ts, dur, name FROM slice"
    unlimited = self._run(sql)
    limited = self._run(sql, limit=10**9)
    ex_u = self._status(unlimited["queryUuid"])
    ex_l = self._status(limited["queryUuid"])
    self.assertEqual(ex_u["status"], "SUCCESS")
    self.assertGreater(ex_u["processedRows"], 0)
    self.assertEqual(ex_u["processedRows"], ex_l["processedRows"])
    self.assertEqual(self._count(ex_u["tableName"]), ex_u["processedRows"])
    types = {c["name"]: c["type"] for c in ex_u["schema"]}
    self.assertEqual(types["ts"], "INT64")
    self.assertEqual(types["name"], "STRING")

  def test_query_failing_everywhere_fails(self):
    status, data, _ = self._request("POST", "/execute_bigtrace_query", {
        "perfetto_sql": "SELECT * FROM no_such_table",
        "limit": 0
    })
    self.assertEqual(status, 400, data)
    execs = self._request("GET", "/query_executions")[1]["queryExecutions"]
    last = next(e for e in execs if e["perfettoSql"].endswith("no_such_table"))
    self.assertEqual(last["status"], "FAILED")
    self.assertIn("no such table", last["errorMessage"])

  def test_per_trace_errors_keep_other_traces(self):
    # abs() overflows, failing the query, on traces with an even slice count.
    sql = ("SELECT CASE WHEN (SELECT count(*) FROM slice) % 2 = 0 "
           "THEN abs(-9223372036854775807 - 1) ELSE 1 END AS v")
    status, data, _ = self._request("POST", "/execute_bigtrace_query", {
        "perfetto_sql": sql,
        "limit": 0
    })
    execs = self._request("GET", "/query_executions")[1]["queryExecutions"]
    ex = next(e for e in execs if e["perfettoSql"] == sql)
    failed = 0
    if ex.get("errorMessage"):
      failed = int(ex["errorMessage"].split(" ")[0])
    self.assertEqual(ex.get("processedRows", 0) + failed, ex["totalTraces"])
    self.assertEqual(ex["status"],
                     "SUCCESS" if failed < ex["totalTraces"] else "FAILED",
                     (status, data))

  def test_untyped_columns_get_types_from_later_traces(self):
    # NULL on the cold trace, numbers elsewhere: the probe cannot type `v`.
    sql = ("SELECT CASE WHEN (SELECT count(*) FROM slice) = "
           "(SELECT count(*) FROM slice WHERE name GLOB '*cold*') "
           "THEN NULL ELSE 7 END AS v")
    data = self._run(sql)
    ex = self._status(data["queryUuid"])
    self.assertEqual(ex["status"], "SUCCESS")
    types = {c["name"]: c["type"] for c in ex["schema"]}
    self.assertEqual(types["v"], "INT64")

  def test_cancel_stops_streaming_query(self):
    if self.server.duckdb.stream_api is None:
      self.skipTest("streaming unavailable")
    status, data, _ = self._request("POST", "/execute_bigtrace_query_async", {
        "perfetto_sql": "SELECT s1.ts FROM slice s1, slice s2",
        "limit": 0,
    })
    self.assertEqual(status, 200, data)
    uid = data["queryUuid"]
    time.sleep(1.0)
    start = time.monotonic()
    self._request("POST", f"/query_executions/{uid}:cancel", {})
    executor = self.server.executor
    wait_for(lambda: not any(r.is_alive() for r in executor._runs), 30)
    self.assertLess(time.monotonic() - start, 15)
    self.assertEqual(self._status(uid)["status"], "CANCELLED")


if __name__ == "__main__":
  unittest.main()
