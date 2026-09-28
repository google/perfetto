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
"""Query results kept in trace processor's encoding until rows are read.

As Python lists, a row of `slice` costs ~1 KB and ~1us per cell to decode, so
millions of rows take GBs, minutes of GIL time and long garbage-collector
pauses. A ResultSet instead keeps each trace's CellsBatch protos (~60 bytes a
row) and decodes only the batches a page of results needs.
"""

import json
import threading
from typing import Any, List, Sequence, Tuple

from perfetto.bigtrace.server import query_result

# (CellsBatch proto, column count, rows kept from its start, trace metadata).
Chunk = Tuple[bytes, int, int, Tuple[Any, ...]]


class ResultSet:
  """Rows of a query: SQL columns from encoded batches, then trace metadata."""

  def __init__(self, chunks: Sequence[Chunk] = ()):
    self._lock = threading.Lock()
    self._chunks: List[Chunk] = list(chunks)
    self._len = sum(c[2] for c in self._chunks)

  def __len__(self) -> int:
    return self._len

  def add(self, batches: Sequence[Tuple[bytes, int]], columns: int,
          meta: Sequence[Any]) -> None:
    """Appends one trace's (batch, row count) pairs."""
    meta = tuple(meta)
    with self._lock:
      for batch, count in batches:
        self._chunks.append((batch, columns, count, meta))
        self._len += count

  def page(self, offset: int, limit: int) -> List[List[Any]]:
    """Returns rows [offset, offset + limit), decoding only what they need."""
    with self._lock:
      chunks = list(self._chunks)
    out: List[List[Any]] = []
    for chunk in chunks:
      count = chunk[2]
      if offset >= count:
        offset -= count
        continue
      if len(out) >= limit:
        break
      rows = _decode(chunk)
      out.extend(rows[offset:offset + limit - len(out)])
      offset = 0
    return out

  def rows(self) -> List[List[Any]]:
    """Decodes and returns every row."""
    with self._lock:
      chunks = list(self._chunks)
    return [r for c in chunks for r in _decode(c)]

  def to_records(self) -> List[Tuple[str, int, int, bytes]]:
    """Returns the chunks in a form the history database stores."""
    with self._lock:
      return [(json.dumps(meta, default=str), cols, count, batch)
              for batch, cols, count, meta in self._chunks]

  @classmethod
  def from_records(
      cls, records: Sequence[Tuple[str, int, int, bytes]]) -> "ResultSet":
    return cls([(bytes(batch), cols, count, tuple(json.loads(meta)))
                for meta, cols, count, batch in records])


def _decode(chunk: Chunk) -> List[List[Any]]:
  batch, columns, count, meta = chunk
  rows = query_result.decode_batch(batch, columns)[:count]
  extra = list(meta)
  return [r + extra for r in rows]
