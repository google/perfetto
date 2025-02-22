# Copyright (C) 2024 The Android Open Source Project
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

import itertools
from typing import List, Sized, Union

from perfetto.common.exceptions import PerfettoException

try:
  import pandas as pd
  HAS_PANDAS = True
except ModuleNotFoundError:
  HAS_PANDAS = False
except ImportError:
  HAS_PANDAS = False

try:
  import numpy as np
  HAS_NUMPY = True
except ModuleNotFoundError:
  HAS_NUMPY = False
except ImportError:
  HAS_NUMPY = False

# Values of these constants correspond to the QueryResponse message at
# protos/perfetto/trace_processor/trace_processor.proto
QUERY_CELL_INVALID_FIELD_ID = 0
QUERY_CELL_NULL_FIELD_ID = 1
QUERY_CELL_VARINT_FIELD_ID = 2
QUERY_CELL_FLOAT64_FIELD_ID = 3
QUERY_CELL_STRING_FIELD_ID = 4
QUERY_CELL_BLOB_FIELD_ID = 5
QUERY_CELL_TYPE_COUNT = 6


def _extract_strings(x: Union[str, bytes]):
  # It's possible on some occasions that there are non UTF-8 characters
  # in the string_cells field. If this is the case, string_cells is
  # a bytestring which needs to be decoded (but passing ignore so that
  # we don't fail in decoding).
  try:
    input: str = x.decode('utf-8', 'ignore')
  except AttributeError:
    # AttributeError can occur when |x| is an str which happens when everything
    # in it is UTF-8 (protobuf automatically does the conversion if it can).
    input: str = x
  res = input.split('\0')
  if res:
    res.pop()
  return res


# Provides a Python interface to operate on the contents of QueryResult protos
class QueryResultIterator(Sized):
  # This is the class returned to the user and contains one row of the
  # resultant query. Each column name is stored as an attribute of this
  # class, with the value corresponding to the column name and row in
  # the query results table.
  class Row(object):
    # Required for pytype to correctly infer attributes from Row objects
    _HAS_DYNAMIC_ATTRIBUTES = True

    def __str__(self):
      return str(self.__dict__)

    def __repr__(self):
      return self.__dict__

  def __init__(self, column_names: List[str], batches: List):
    self.column_names = list(column_names)
    self.column_count = len(column_names)

    if batches and not batches[-1].is_last_batch:
      raise PerfettoException('Last batch did not have is_last_batch flag set')

    self.cell_count = sum(len(b.cells) for b in batches)
    for b in batches:
      if self.column_count > 0 and len(b.cells) % self.column_count != 0:
        raise PerfettoException(
            f"Result has {self.cell_count} cells, not divisible by {self.column_count} columns"
        )

    self.row_count = self.cell_count // self.column_count if self.column_count > 0 else 0
    self.cell_index = 0

    if HAS_NUMPY:
      self.cells = np.empty(self.cell_count, dtype='object')
      cell_count = 0
      for b in batches:
        all_cells = np.array(b.cells)
        all_cells_count = len(all_cells)
        cut = self.cells[cell_count:cell_count + all_cells_count]
        cut[all_cells == QUERY_CELL_NULL_FIELD_ID] = None
        cut[all_cells == QUERY_CELL_VARINT_FIELD_ID] = b.varint_cells
        cut[all_cells == QUERY_CELL_FLOAT64_FIELD_ID] = b.float64_cells
        cut[all_cells == QUERY_CELL_STRING_FIELD_ID] = _extract_strings(
            b.string_cells)
        cut[all_cells == QUERY_CELL_BLOB_FIELD_ID] = b.blob_cells
        cell_count += all_cells_count
    else:
      self.cells = [None] * self.cell_count
      cells = [
          [],
          [],
          list(itertools.chain.from_iterable(b.varint_cells for b in batches)),
          list(itertools.chain.from_iterable(b.float64_cells for b in batches)),
          list(
              itertools.chain.from_iterable(
                  _extract_strings(b.string_cells) for b in batches)),
          list(itertools.chain.from_iterable(b.blob_cells for b in batches)),
      ]
      cell_offsets = [0] * (QUERY_CELL_BLOB_FIELD_ID + 1)
      for i, ct in enumerate(
          itertools.chain.from_iterable(b.cells for b in batches)):
        self.cells[i] = cells[ct][
            cell_offsets[ct]] if ct != QUERY_CELL_NULL_FIELD_ID else None
        cell_offsets[ct] += 1

  # To use the query result as a populated Pandas dataframe, this
  # function must be called directly after calling query inside
  # TraceProcessor / Bigtrace.
  def as_pandas_dataframe(self):
    if HAS_PANDAS and HAS_NUMPY:
      assert isinstance(self.cells, np.ndarray)
      return pd.DataFrame(
          self.cells.reshape((self.row_count, self.column_count)),
          columns=self.column_names)
    else:
      raise PerfettoException(
          'pandas/numpy dependency missing. Please run `pip3 install pandas numpy`'
      )

  def __len__(self):
    return self.row_count

  def __iter__(self):
    return self

  def __next__(self):
    if self.cell_index == self.cell_count:
      raise StopIteration
    result = QueryResultIterator.Row()
    for i, column_name in enumerate(self.column_names):
      setattr(result, column_name, self.cells[self.cell_index + i])
    self.cell_index += self.column_count
    return result
