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
"""Streams per-trace query results from trace processors into DuckDB.

Python only orchestrates: for each trace it hands the extension the address of
a loaded trace_processor_shell. The extension (proto_ingest.cc) POSTs the
query, decodes the reply as it arrives and queues cells that an
`INSERT ... FROM pf_scan_tp_stream()` drains into the result table.

A query's rows go through a sequence of segments, each one stream plus one
INSERT. Segments rotate every few seconds or traces so that partial results
become visible while the query runs. The first segment starts taking feeds
before the column types are known; its INSERT starts once the cells seen so
far settle them (see pf_tp_stream_wait_typed).
"""

import ctypes
import dataclasses as dc
import logging
import os
import threading
import time
from typing import Callable, List, Optional, Sequence, Set

logger = logging.getLogger(__name__)

FEED_OK = 0
FEED_SQL_ERROR = 1
FEED_COLUMN_MISMATCH = 2
FEED_IO_ERROR = 3
FEED_CANCELLED = 4
FEED_CLOSED = 5
FEED_BAD_HANDLE = 6
FEED_LIMIT = 7

_MSG_CAP = 4096
_QUEUE_CHUNKS = 512

# CellsBatch.CellType values.
_CELL_VARINT = 2
_CELL_FLOAT64 = 3
_CELL_STRING = 4
_CELL_BLOB = 5


@dc.dataclass
class FeedResult:
  status: int
  rows: int = 0
  message: str = ""

  @property
  def ok(self) -> bool:
    """Whether all the trace's rows (up to the row limit) were queued."""
    return self.status in (FEED_OK, FEED_LIMIT)


def type_from_cell_types(mask: int) -> Optional[str]:
  """The column type holding every cell type in `mask`; None if no cells."""
  if mask & ((1 << _CELL_STRING) | (1 << _CELL_BLOB)):
    return "STRING"
  if mask & (1 << _CELL_FLOAT64):
    return "DOUBLE"
  if mask & (1 << _CELL_VARINT):
    return "INT64"
  return None


class TpStreamApi:
  """ctypes binding of the extension's `pf_tp_stream_*` C ABI.

  Uses the library DuckDB already loaded (dlopen returns the same handle for
  the same file), so streams opened here are visible to `pf_scan_tp_stream`.
  Calls release the GIL, so feeds block only their own thread.
  """

  def __init__(self, lib_path: str):
    lib = ctypes.CDLL(lib_path)
    i32, i64 = ctypes.c_int32, ctypes.c_int64
    c_char_pp = ctypes.POINTER(ctypes.c_char_p)
    sigs = {
        "pf_tp_stream_open": ([ctypes.c_char_p, c_char_pp, i32, i32, i64], i64),
        "pf_tp_stream_feed": ([
            i64, ctypes.c_char_p, i32, i32,
            ctypes.POINTER(i64), ctypes.c_char_p, i32
        ], i32),
        "pf_tp_stream_wait_typed": ([i64, i64], i32),
        "pf_tp_stream_columns": ([i64, ctypes.c_char_p, i32], i32),
        "pf_tp_stream_cell_types": ([i64,
                                     ctypes.POINTER(ctypes.c_uint32),
                                     i32], None),
        "pf_tp_stream_close": ([i64], None),
        "pf_tp_stream_cancel": ([i64], None),
        "pf_tp_stream_free": ([i64], None),
        "pf_query_progress_reset": ([], None),
        "pf_query_progress_get": ([
            ctypes.POINTER(i64),
            ctypes.POINTER(i64),
            ctypes.POINTER(i64),
            ctypes.POINTER(i64),
        ], i32),
    }
    for name, (args, res) in sigs.items():
      if hasattr(lib, name):
        fn = getattr(lib, name)
        fn.argtypes = args
        fn.restype = res
    self._lib = lib

  def open(self,
           sql: str,
           columns: Optional[Sequence[str]] = None,
           row_limit: int = 0,
           queue_chunks: int = _QUEUE_CHUNKS) -> int:
    """Opens a stream; `columns` None adopts the first reply's columns."""
    cols = [c.encode("utf-8") for c in columns or []]
    arr = (ctypes.c_char_p * max(1, len(cols)))(*cols)
    return int(
        self._lib.pf_tp_stream_open(
            sql.encode("utf-8"), arr, -1 if columns is None else len(cols),
            queue_chunks, row_limit))

  def feed(self,
           handle: int,
           host: str,
           port: int,
           trace_ord: Optional[int] = None) -> FeedResult:
    if trace_ord is None:
      target = host
      ord_val = port
      if target.startswith("tcp:"):
        target = target[4:]
      h, p = target.rsplit(":", 1)
      return self.feed(handle, h, int(p), ord_val)
    rows = ctypes.c_int64(0)
    msg = ctypes.create_string_buffer(_MSG_CAP)
    status = self._lib.pf_tp_stream_feed(handle,
                                         host.encode("utf-8"), port, trace_ord,
                                         ctypes.byref(rows), msg, _MSG_CAP)
    return FeedResult(
        int(status), int(rows.value), msg.value.decode("utf-8", "replace"))

  def wait_typed(self, handle: int, timeout_ms: int) -> bool:
    return self._lib.pf_tp_stream_wait_typed(handle, timeout_ms) == 1

  def columns(self, handle: int) -> Optional[List[str]]:
    size = 1 << 16
    while True:
      buf = ctypes.create_string_buffer(size)
      n = self._lib.pf_tp_stream_columns(handle, buf, size)
      if n == -2:
        size *= 4
        continue
      if n < 0:
        return None
      names = buf.raw.split(b"\0")[:n]
      return [c.decode("utf-8", "replace") for c in names]

  def cell_types(self, handle: int, n: int) -> List[int]:
    """Per column, the bitmask (1 << CellType) of the cells seen."""
    out = (ctypes.c_uint32 * max(1, n))()
    self._lib.pf_tp_stream_cell_types(handle, out, n)
    return list(out)[:n]

  def close(self, handle: int) -> None:
    self._lib.pf_tp_stream_close(handle)

  def cancel(self, handle: int) -> None:
    self._lib.pf_tp_stream_cancel(handle)

  def free(self, handle: int) -> None:
    self._lib.pf_tp_stream_free(handle)

  def reset_query_progress(self) -> None:
    if hasattr(self._lib, "pf_query_progress_reset"):
      self._lib.pf_query_progress_reset()

  def get_query_progress(self) -> int:
    if not hasattr(self._lib, "pf_query_progress_get"):
      return -1
    r = ctypes.c_int64(0)
    ing = ctypes.c_int64(0)
    done = ctypes.c_int64(0)
    live = ctypes.c_int64(0)
    return int(
        self._lib.pf_query_progress_get(
            ctypes.byref(r),
            ctypes.byref(ing),
            ctypes.byref(done),
            ctypes.byref(live),
        ))


class _Segment:
  """One stream and the INSERT draining it."""

  def __init__(self, handle: int):
    self.handle = handle
    self.fed = 0
    self.started_at = time.monotonic()
    self.thread: Optional[threading.Thread] = None
    self.error: Optional[Exception] = None
    self.queued_rows = 0
    self.inserted_rows = 0
    # Traces whose rows all went to this segment.
    self.ok_ords: List[int] = []


@dc.dataclass
class FinishResult:
  errors: List[str]
  # Traces whose rows were lost with a failed INSERT.
  failed_ords: Set[int]
  # Per column, the cell types seen over the whole query.
  cell_types: List[int]

  def __iter__(self):
    return iter(self.errors)

  def __len__(self):
    return len(self.errors)

  def __eq__(self, other):
    if isinstance(other, list):
      return self.errors == other
    return super().__eq__(other)


# Called once the columns and types are known; creates the result table and
# returns insert(handle), which drains a stream into it and returns the rows
# inserted.
OnSchema = Callable[[List[str], List[str]], Callable[[int], int]]


class StreamIngest:
  """Streams one query's per-trace results into one DuckDB table.

  feed() once per trace from any thread, then finish() (after cancel() to
  stop early). Columns no cell typed before the INSERT started are STRING;
  `types` and `untyped` say which.
  """

  def __init__(self,
               api: TpStreamApi,
               sql: str,
               on_schema: OnSchema,
               columns: Optional[Sequence[str]] = None,
               row_limit: int = 0,
               segment_traces: int = 256,
               segment_secs: float = 10.0):
    self.api = api
    self.sql = sql
    self.row_limit = row_limit
    self._on_schema = on_schema
    # A row limit spans the whole query, so it needs a single stream.
    self.segment_traces = segment_traces if row_limit <= 0 else 1 << 62
    self.segment_secs = segment_secs if row_limit <= 0 else float("inf")
    self.columns: Optional[List[str]] = list(
        columns) if columns is not None else None
    self.types: List[str] = []
    self.untyped: List[str] = []
    self.cell_types: List[int] = []
    self._insert: Optional[Callable[[int], int]] = None
    self._lock = threading.Lock()
    self._started = threading.Event()
    self._force = threading.Event()
    self._cancelled = False
    self._finished = False
    self._schema_error: Optional[Exception] = None
    first = _Segment(api.open(sql, columns, row_limit))
    self._segments: List[_Segment] = [first]
    self._current = first
    self._starter = threading.Thread(
        target=self._start_when_typed,
        name="bigtrace-stream-start",
        daemon=True)
    self._starter.start()

  @property
  def started(self) -> bool:
    return self._started.is_set()

  def start(self, *args, **kwargs) -> None:
    pass

  def feed(self,
           host: str,
           port: int,
           trace_ord: Optional[int] = None) -> FeedResult:
    """Ingests one trace's result; blocks until all its rows are queued."""
    if trace_ord is None:
      target = host
      ord_val = port
      if target.startswith("tcp:"):
        target = target[4:]
      h, p = target.rsplit(":", 1)
      return self.feed(h, int(p), ord_val)
    while True:
      with self._lock:
        if self._cancelled or self._finished:
          return FeedResult(FEED_CANCELLED)
        seg = self._current
        if self._started.is_set() and (seg.fed >= self.segment_traces or
                                       time.monotonic() - seg.started_at
                                       >= self.segment_secs):
          self.api.close(seg.handle)
          seg = self._new_segment_locked()
        seg.fed += 1
      res = self.api.feed(seg.handle, host, port, trace_ord)
      with self._lock:
        seg.queued_rows += res.rows
        if res.ok:
          seg.ok_ords.append(trace_ord)
        error = seg.error or self._schema_error
      if res.status in (FEED_CLOSED, FEED_BAD_HANDLE) and error is None:
        continue  # Rotated away; feed the next segment.
      if res.status in (FEED_CLOSED, FEED_BAD_HANDLE, FEED_CANCELLED) and error:
        return FeedResult(FEED_IO_ERROR, res.rows, f"DuckDB insert failed: "
                          f"{error}")
      return res

  def force_start(self) -> None:
    """Starts inserting even if some columns have no typed cell yet."""
    self._force.set()

  def cancel(self) -> None:
    """Stops all feeds and inserts promptly; rows inserted so far stay."""
    with self._lock:
      self._cancelled = True
      segs = list(self._segments)
    self._force.set()
    for seg in segs:
      self.api.cancel(seg.handle)

  def finish(self) -> FinishResult:
    """Ends all segments and waits for their inserts."""
    with self._lock:
      self._finished = True
      segs = list(self._segments)
    for seg in segs:
      self.api.close(seg.handle)
    self._force.set()
    self._starter.join()
    with self._lock:
      segs = list(self._segments)
    errors: List[str] = []
    failed: Set[int] = set()
    masks = [0] * len(self.columns or [])
    if self._schema_error is not None:
      errors.append(f"could not create the result table: "
                    f"{self._schema_error}")
    for seg in segs:
      self.api.close(seg.handle)
      if seg.thread is not None:
        seg.thread.join()
      for i, m in enumerate(self.api.cell_types(seg.handle, len(masks))):
        masks[i] |= m
      self.api.free(seg.handle)
      lost = seg.error is not None or (seg.thread is None and seg.queued_rows)
      if not lost and seg.inserted_rows != seg.queued_rows and (
          not self._cancelled):
        logger.error("Stream %d queued %d rows but inserted %d", seg.handle,
                     seg.queued_rows, seg.inserted_rows)
        lost = True
      if lost:
        failed.update(seg.ok_ords)
        if seg.error is not None:
          errors.append(f"DuckDB insert failed: {seg.error}")
    self.cell_types = masks
    return FinishResult(errors, failed, masks)

  def _new_segment_locked(self) -> _Segment:
    seg = _Segment(self.api.open(self.sql, self.columns, self.row_limit))
    self._segments.append(seg)
    self._current = seg
    self._start_insert(seg)
    return seg

  def _start_insert(self, seg: _Segment) -> None:
    seg.thread = threading.Thread(
        target=self._run_insert,
        args=(seg,),
        name=f"bigtrace-insert-{seg.handle}",
        daemon=True)
    seg.thread.start()

  def _start_when_typed(self) -> None:
    first = self._segments[0]
    while True:
      typed = self.api.wait_typed(first.handle, 200)
      with self._lock:
        if self._cancelled:
          return
      if not typed and not self._force.is_set():
        continue
      columns = self.api.columns(first.handle)
      if columns is None:
        if self._force.is_set():
          return  # No trace returned a result: there is no table.
        continue
      masks = self.api.cell_types(first.handle, len(columns))
      types = [type_from_cell_types(m) or "STRING" for m in masks]
      try:
        insert = self._on_schema(columns, types)
      except Exception as e:  # pylint: disable=broad-except
        logger.warning("Could not create the result table: %s", e)
        with self._lock:
          self._schema_error = e
        self.cancel()
        return
      with self._lock:
        self.columns = columns
        self.types = types
        self.untyped = [
            c for c, m in zip(columns, masks) if type_from_cell_types(m) is None
        ]
        self._insert = insert
        self._start_insert(first)
        self._started.set()
      return

  def _run_insert(self, seg: _Segment) -> None:
    assert self._insert is not None
    try:
      rows = self._insert(seg.handle)
      with self._lock:
        seg.inserted_rows = rows
    except Exception as e:  # pylint: disable=broad-except
      with self._lock:
        cancelled = self._cancelled
        seg.error = e
      if not cancelled:
        logger.warning("Stream insert failed: %s", e)
      # Unblocks feeds waiting on a queue nobody drains anymore.
      self.api.cancel(seg.handle)


def default_scan_threads() -> int:
  """DuckDB threads per INSERT; the DuckDB sink bounds streaming throughput."""
  return min(64, max(4, (os.cpu_count() or 8) // 2))
