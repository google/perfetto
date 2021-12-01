# Copyright (C) 2020 The Android Open Source Project
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

from urllib.parse import urlparse

from .http import TraceProcessorHttp
from .loader import get_loader
from .protos import ProtoFactory
from .shell import load_shell


# Custom exception raised if any trace_processor functions return a
# response with an error defined
class TraceProcessorException(Exception):

  def __init__(self, message):
    super().__init__(message)


class TraceProcessor:

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

    def __str__(self):
      return str(self.__dict__)

    def __repr__(self):
      return self.__dict__

  class QueryResultIterator:

    def __init__(self, column_names, batches):
      self.__column_names = column_names
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
        # Null-terminated strings in a batch are concatenated
        # into a single large byte array, so we split on the
        # null-terminator to get the individual strings
        strings_batch = batches[batch_index].string_cells.split('\0')[:-1]
        self.__data_lists[TraceProcessor.QUERY_CELL_STRING_FIELD_ID].extend(
            strings_batch)
        self.__data_lists[TraceProcessor.QUERY_CELL_VARINT_FIELD_ID].extend(
            batches[batch_index].varint_cells)
        self.__data_lists[TraceProcessor.QUERY_CELL_FLOAT64_FIELD_ID].extend(
            batches[batch_index].float64_cells)
        self.__data_lists[TraceProcessor.QUERY_CELL_BLOB_FIELD_ID].extend(
            batches[batch_index].blob_cells)
        self.__cells.extend(batches[batch_index].cells)

        if batches[batch_index].is_last_batch:
          break
        batch_index += 1

      # If there are no rows in the query result, exit since
      # we won't be returning anything and don't need to go
      # through the process of generating the mapping between
      # columns and data_list indices
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
        raise TraceProcessorException("Cell count " + str(len(self.__cells)) +
                                      " is not a multiple of column count " +
                                      str(len(self.__column_names)))

    # To use the query result as a populated Pandas dataframe, this
    # function must be called directly after calling query inside
    # TraceProcesor.
    def as_pandas_dataframe(self):
      try:
        import numpy as np
        import pandas as pd

        # Populate the dataframe with the query results
        rows = []
        for i in range(0, self.__count):
          row = []
          base_cell_index = i * self.__column_count
          for num in range(len(self.__column_names)):
            col_type = self.__cells[base_cell_index + num]
            if col_type == TraceProcessor.QUERY_CELL_INVALID_FIELD_ID:
              raise TraceProcessorException('Invalid cell type')

            if col_type == TraceProcessor.QUERY_CELL_NULL_FIELD_ID:
              row.append(None)
            else:
              col_index = self.__data_lists_index[col_type]
              self.__data_lists_index[col_type] += 1
              row.append(self.__data_lists[col_type][col_index])
          rows.append(row)

        df = pd.DataFrame(rows, columns=self.__column_names)
        return df.where(df.notnull(), None).reset_index(drop=True)

      except ModuleNotFoundError:
        raise TraceProcessorException(
            'Python dependencies missing. Please pip3 install pandas numpy')

    def __len__(self):
      return self.__count

    def __iter__(self):
      return self

    def __next__(self):
      if self.__current_index == self.__count:
        raise StopIteration
      result = TraceProcessor.Row()
      base_cell_index = self.__current_index * self.__column_count
      for num, column_name in enumerate(self.__column_names):
        col_type = self.__cells[base_cell_index + num]
        if col_type == TraceProcessor.QUERY_CELL_INVALID_FIELD_ID:
          raise TraceProcessorException('Invalid cell type')
        if col_type != TraceProcessor.QUERY_CELL_NULL_FIELD_ID:
          col_index = self.__data_lists_index[col_type]
          self.__data_lists_index[col_type] += 1
          setattr(result, column_name, self.__data_lists[col_type][col_index])
        else:
          setattr(result, column_name, None)

      self.__current_index += 1
      return result

  def __init__(self,
               addr=None,
               file_path=None,
               bin_path=None,
               unique_port=True,
               verbose=False):
    # Load trace_processor_shell or access via given address
    if addr:
      p = urlparse(addr)
      tp = TraceProcessorHttp(p.netloc if p.netloc else p.path)
    else:
      url, self.subprocess = load_shell(
          bin_path=bin_path, unique_port=unique_port, verbose=verbose)
      tp = TraceProcessorHttp(url)
    self.http = tp
    self.protos = ProtoFactory()

    # Parse trace by its file_path into the loaded instance of trace_processor
    if file_path:
      get_loader().parse_file(self.http, file_path)

  def query(self, sql):
    """Executes passed in SQL query using class defined HTTP API, and returns
    the response as a QueryResultIterator. Raises TraceProcessorException if
    the response returns with an error.

    Args:
      sql: SQL query written as a String

    Returns:
      A class which can iterate through each row of the results table. This
      can also be converted to a pandas dataframe by calling the
      as_pandas_dataframe() function after calling query.
    """
    response = self.http.execute_query(sql)
    if response.error:
      raise TraceProcessorException(response.error)

    return TraceProcessor.QueryResultIterator(response.column_names,
                                              response.batch)

  def metric(self, metrics):
    """Returns the metrics data corresponding to the passed in trace metric.
    Raises TraceProcessorException if the response returns with an error.

    Args:
      metrics: A list of valid metrics as defined in TraceMetrics

    Returns:
      The metrics data as a proto message
    """
    response = self.http.compute_metric(metrics)
    if response.error:
      raise TraceProcessorException(response.error)

    metrics = self.protos.TraceMetrics()
    metrics.ParseFromString(response.metrics)
    return metrics

  def enable_metatrace(self):
    """Enable metatrace for the currently running trace_processor.
    """
    return self.http.enable_metatrace()

  def disable_and_read_metatrace(self):
    """Disable and return the metatrace formed from the currently running
    trace_processor. This must be enabled before attempting to disable. This
    returns the serialized bytes of the metatrace data directly. Raises
    TraceProcessorException if the response returns with an error.
    """
    response = self.http.disable_and_read_metatrace()
    if response.error:
      raise TraceProcessorException(response.error)

    return response.metatrace

  def __enter__(self):
    return self

  def __exit__(self, _, __, ___):
    self.close()
    return False

  def close(self):
    if hasattr(self, 'subprocess'):
      self.subprocess.kill()
    self.http.conn.close()
