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

from perfetto.common.exceptions import PerfettoException


# Provides a Python interface to operate on the contents of QueryResult protos
class QueryResultIterator:
  # Values of these constants correspond to the QueryResponse message at
  # protos/perfetto/trace_processor/trace_processor.proto
  QUERY_CELL_INVALID_FIELD_ID = 0
  QUERY_CELL_NULL_FIELD_ID = 1
  QUERY_CELL_VARINT_FIELD_ID = 2
  QUERY_CELL_FLOAT64_FIELD_ID = 3
  QUERY_CELL_STRING_FIELD_ID = 4
  QUERY_CELL_BLOB_FIELD_ID = 5

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

  def __init__(self, column_names, batches):
    self.__column_names = list(column_names)
    self.__column_count = 0
    self.__count = 0
    self.__cells = []
    self.__data_lists = [[], [], [], [], [], []]
    self.__data_lists_index = [0, 0, 0, 0, 0, 0]
    self.__current_index = 0

    # Iterate over all the batches and collect their
    # contents into lists based on the type of the batch
    batch_index = 0
    while True:
      # It's possible on some occasions that there are non UTF-8 characters
      # in the string_cells field. If this is the case, string_cells is
      # a bytestring which needs to be decoded (but passing ignore so that
      # we don't fail in decoding).
      strings_batch_str = batches[batch_index].string_cells
      try:
        strings_batch_str = strings_batch_str.decode('utf-8', 'ignore')
      except AttributeError:
        # AttributeError can occur when |strings_batch_str| is an str which
        # happens when everything in it is UTF-8 (protobuf automatically
        # does the conversion if it can).
        pass

      # Null-terminated strings in a batch are concatenated
      # into a single large byte array, so we split on the
      # null-terminator to get the individual strings
      strings_batch = strings_batch_str.split('\0')[:-1]
      self.__data_lists[QueryResultIterator.QUERY_CELL_STRING_FIELD_ID].extend(
          strings_batch)
      self.__data_lists[QueryResultIterator.QUERY_CELL_VARINT_FIELD_ID].extend(
          batches[batch_index].varint_cells)
      self.__data_lists[QueryResultIterator.QUERY_CELL_FLOAT64_FIELD_ID].extend(
          batches[batch_index].float64_cells)
      self.__data_lists[QueryResultIterator.QUERY_CELL_BLOB_FIELD_ID].extend(
          batches[batch_index].blob_cells)
      self.__cells.extend(batches[batch_index].cells)

      if batches[batch_index].is_last_batch:
        break

      batch_index += 1

    # If there are no rows in the query result, don't bother updating the
    # counts to avoid dealing with / 0 errors.
    if len(self.__cells) == 0:
      return

    # The count we collected so far was a count of all individual columns
    # in the query result, so we divide by the number of columns in a row
    # to get the number of rows
    self.__column_count = len(self.__column_names)
    self.__count = int(len(self.__cells) / self.__column_count)

    # Data integrity check - see that we have the expected amount of cells
    # for the number of rows that we need to return
    if len(self.__cells) % self.__column_count != 0:
      raise PerfettoException("Cell count " + str(len(self.__cells)) +
                              " is not a multiple of column count " +
                              str(len(self.__column_names)))

  # To use the query result as a populated Pandas dataframe, this
  # function must be called directly after calling query inside
  # TraceProcessor / Bigtrace.
  def as_pandas_dataframe(self):
    try:
      import pandas as pd

      # Populate the dataframe with the query results
      rows = []
      for i in range(0, self.__count):
        row = []
        base_cell_index = i * self.__column_count
        for num in range(len(self.__column_names)):
          col_type = self.__cells[base_cell_index + num]
          if col_type == QueryResultIterator.QUERY_CELL_INVALID_FIELD_ID:
            raise PerfettoException('Invalid cell type')

          if col_type == QueryResultIterator.QUERY_CELL_NULL_FIELD_ID:
            row.append(None)
          else:
            col_index = self.__data_lists_index[col_type]
            self.__data_lists_index[col_type] += 1
            row.append(self.__data_lists[col_type][col_index])
        rows.append(row)

      df = pd.DataFrame(rows, columns=self.__column_names)
      return df.astype(object).where(df.notnull(), None).reset_index(drop=True)

    except ModuleNotFoundError:
      raise PerfettoException(
          'Python dependencies missing. Please pip3 install pandas numpy')

  def __len__(self):
    return self.__count

  def __iter__(self):
    return self

  def __next__(self):
    if self.__current_index == self.__count:
      raise StopIteration
    result = QueryResultIterator.Row()
    base_cell_index = self.__current_index * self.__column_count
    for num, column_name in enumerate(self.__column_names):
      col_type = self.__cells[base_cell_index + num]
      if col_type == QueryResultIterator.QUERY_CELL_INVALID_FIELD_ID:
        raise PerfettoException('Invalid cell type')
      if col_type != QueryResultIterator.QUERY_CELL_NULL_FIELD_ID:
        col_index = self.__data_lists_index[col_type]
        self.__data_lists_index[col_type] += 1
        setattr(result, column_name, self.__data_lists[col_type][col_index])
      else:
        setattr(result, column_name, None)

    self.__current_index += 1
    return result
