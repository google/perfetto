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
"""Fast, incremental decoding of trace processor query results.

TraceProcessor.query() decodes the whole QueryResult proto up front, which is
slow with the pure-Python protobuf runtime (~20us per row of `slice`) and holds
the GIL, stalling every other query and HTTP request. This splits the proto
into its encoded batches, which callers decode only when they need the rows.
"""

import base64
import struct
from typing import Any, Iterator, List, NamedTuple, Tuple

from perfetto.trace_processor.api import TraceProcessor, TraceProcessorException

# Field numbers from protos/perfetto/trace_processor/trace_processor.proto.
_QUERY_ARGS_SQL = 1
_RESULT_COLUMN_NAMES = 1
_RESULT_ERROR = 2
_RESULT_BATCH = 3
_RESULT_STATEMENT_COUNT = 4
_RESULT_STATEMENT_WITH_OUTPUT_COUNT = 5
_BATCH_CELLS = 1
_BATCH_VARINTS = 2
_BATCH_FLOAT64S = 3
_BATCH_BLOBS = 4
_BATCH_STRINGS = 5

# CellsBatch.CellType values.
_CELL_NULL = 1
_CELL_VARINT = 2
_CELL_FLOAT64 = 3
_CELL_STRING = 4
_CELL_BLOB = 5


class QueryResult(NamedTuple):
  columns: List[str]
  # Encoded CellsBatch protos; see decode_batch().
  batches: List[memoryview]
  statement_count: int
  statement_with_output_count: int

  @property
  def is_stateful(self) -> bool:
    """Whether the SQL ran statements without output (CREATE, INCLUDE, ...).

    Trace processor also counts a final SELECT that returned no rows as
    having no output, so that one is not counted.
    """
    without_output = self.statement_count - self.statement_with_output_count
    if self.columns and not any(
        row_count(b, len(self.columns)) for b in self.batches):
      without_output -= 1
    return without_output > 0


def query(tp: TraceProcessor, sql: str) -> QueryResult:
  """Runs `sql` and returns its result with the batches still encoded.

  Decode batches with decode_batch(), so callers only pay for the rows they
  need. Raises TraceProcessorException if the query fails.
  """
  sql_bytes = sql.encode("utf-8")
  body = bytes([_QUERY_ARGS_SQL << 3 | 2]) + _varint(len(sql_bytes)) + sql_bytes
  conn = tp.http.conn
  conn.request("POST", "/query", body=body)
  with conn.getresponse() as response:
    data = response.read()

  columns: List[str] = []
  batches: List[memoryview] = []
  counts = {_RESULT_STATEMENT_COUNT: 0, _RESULT_STATEMENT_WITH_OUTPUT_COUNT: 0}
  for field, value in _fields(memoryview(data)):
    if field == _RESULT_COLUMN_NAMES:
      columns.append(bytes(value).decode("utf-8", "replace"))
    elif field == _RESULT_ERROR and len(value):
      raise TraceProcessorException(bytes(value).decode("utf-8", "replace"))
    elif field == _RESULT_BATCH:
      batches.append(value)
    elif field in counts:
      counts[field] = value
  return QueryResult(columns, batches, counts[_RESULT_STATEMENT_COUNT],
                     counts[_RESULT_STATEMENT_WITH_OUTPUT_COUNT])


def row_count(batch: memoryview, column_count: int) -> int:
  """Returns the number of rows in a batch without decoding it."""
  if column_count == 0:
    return 0
  for field, value in _fields(batch):
    if field == _BATCH_CELLS:
      # Cell types are all below 128, so each packed varint is one byte.
      return len(value) // column_count
  return 0


def decode_batch(data: bytes, column_count: int) -> List[List[Any]]:
  """Decodes a batch into rows. Blobs are base64 so values are JSON-safe."""
  cells = b""
  varints: List[int] = []
  float64s: Tuple[float, ...] = ()
  blobs: List[str] = []
  strings: List[str] = []
  for field, value in _fields(memoryview(data)):
    if field == _BATCH_CELLS:
      cells = bytes(value)
    elif field == _BATCH_VARINTS:
      varints = _packed_varints(value)
    elif field == _BATCH_FLOAT64S:
      float64s = struct.unpack(f"<{len(value) // 8}d", value)
    elif field == _BATCH_BLOBS:
      blobs.append(base64.b64encode(value).decode("ascii"))
    elif field == _BATCH_STRINGS:
      # NUL-terminated strings, concatenated.
      strings = bytes(value).decode("utf-8", "replace").split("\0")[:-1]

  sources = {
      _CELL_VARINT: iter(varints).__next__,
      _CELL_FLOAT64: iter(float64s).__next__,
      _CELL_STRING: iter(strings).__next__,
      _CELL_BLOB: iter(blobs).__next__,
  }
  null = lambda: None
  next_value = [sources.get(t, null) for t in range(_CELL_BLOB + 1)]
  values = [next_value[t]() for t in cells]
  if column_count == 0:
    return []
  return [
      values[i:i + column_count] for i in range(0, len(values), column_count)
  ]


def _fields(data: memoryview) -> Iterator[Tuple[int, Any]]:
  """Yields (field number, value) for each field of a proto message.

  Length-delimited values are memoryviews; others are ints (fixed-width ones
  are left undecoded, as none of the fields read here use them).
  """
  pos, end = 0, len(data)
  while pos < end:
    tag, pos = _read_varint(data, pos)
    wire_type = tag & 7
    if wire_type == 0:
      value, pos = _read_varint(data, pos)
    elif wire_type == 2:
      size, pos = _read_varint(data, pos)
      value, pos = data[pos:pos + size], pos + size
    elif wire_type == 1:
      value, pos = 0, pos + 8
    elif wire_type == 5:
      value, pos = 0, pos + 4
    else:
      raise TraceProcessorException(
          f"Malformed query result (wire type {wire_type})")
    yield tag >> 3, value


def _read_varint(data: memoryview, pos: int) -> Tuple[int, int]:
  value = shift = 0
  while True:
    b = data[pos]
    pos += 1
    value |= (b & 0x7F) << shift
    if b < 0x80:
      return value, pos
    shift += 7


def _packed_varints(data: memoryview) -> List[int]:
  """Decodes packed int64s, the hot loop when decoding results."""
  out: List[int] = []
  append = out.append
  value = shift = 0
  for b in bytes(data):
    value |= (b & 0x7F) << shift
    if b < 0x80:
      # int64 is two's complement over 64 bits.
      append(value - (1 << 64) if value >= 1 << 63 else value)
      value = shift = 0
    else:
      shift += 7
  return out


def _varint(value: int) -> bytes:
  out = bytearray()
  while value >= 0x80:
    out.append(value & 0x7F | 0x80)
    value >>= 7
  out.append(value)
  return bytes(out)
