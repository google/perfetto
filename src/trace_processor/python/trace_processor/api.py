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
  # Value 0 corresponds to CELL_INVALID, which is represented as None in
  # this class
  QUERY_CELL_NULL_FIELD_ID = 1
  QUERY_CELL_VARINT_FIELD_ID = 2
  QUERY_CELL_FLOAT64_FIELD_ID = 3
  QUERY_CELL_STRING_FIELD_ID = 4
  QUERY_CELL_BLOB_FIELD_ID = 5

  # This is the class returned to the user and contains one row of the
  # resultant query. Each column name is stored as an attribute of this
  # class, with the value corresponding to the column name and row in
  # the query results table.
  class Row:
    pass

  class QueryResultIterator:

    def __init__(self, column_names, batches):
      self.__batches = batches
      self.__column_names = column_names
      self.__batch_index = 0
      self.__next_index = 0
      # TODO(aninditaghosh): Revisit string cells for larger traces
      self.__string_cells = batches[0].string_cells.split('\0')

    def get_cell_list(self, proto_index):
      if proto_index == TraceProcessor.QUERY_CELL_NULL_FIELD_ID:
        return None
      elif proto_index == TraceProcessor.QUERY_CELL_VARINT_FIELD_ID:
        return self.__batches[self.__batch_index].varint_cells
      elif proto_index == TraceProcessor.QUERY_CELL_FLOAT64_FIELD_ID:
        return self.__batches[self.__batch_index].float64_cells
      elif proto_index == TraceProcessor.QUERY_CELL_STRING_FIELD_ID:
        return self.__string_cells
      elif proto_index == TraceProcessor.QUERY_CELL_BLOB_FIELD_ID:
        return self.__batches[self.__batch_index].blob_cells
      else:
        raise TraceProcessorException('Invalid cell type')

    def cells(self):
      return self.__batches[self.__batch_index].cells

    # To use the query result as a populated Pandas dataframe, this
    # function must be called directly after calling query inside
    # TraceProcesor.
    def as_pandas(self):
      try:
        import numpy as np
        import pandas as pd

        next_index = 0
        df = pd.DataFrame(columns=self.__column_names)
        # TODO(aninditaghosh): Revisit string cells for larger traces
        self.__string_cells = self.__batches[0].string_cells.split('\0')

        # Populate the dataframe with the query results
        while True:
          # If all cells are read, then check if last batch before
          # returning the populated dataframe
          if next_index >= len(self.__batches[self.__batch_index].cells):
            if self.__batches[self.__batch_index].is_last_batch:
              ordered_df = df.reset_index(drop=True)
              return ordered_df
            self.__batch_index += 1
            next_index = 0
            self.__string_cells = self.__batches[
                self.__batch_index].string_cells.split('\0')

          row = []
          for num, column_name in enumerate(self.__column_names):
            cell_list = self.get_cell_list(
                self.__batches[self.__batch_index].cells[next_index + num])
            if cell_list is None:
              row.append(np.NAN)
            else:
              row.append(cell_list.pop(0))
          df.loc[-1] = row
          df.index = df.index + 1
          next_index = next_index + len(self.__column_names)

      except ModuleNotFoundError:
        raise TraceProcessorException(
            'The sufficient libraries are not installed')

    def __iter__(self):
      return self

    def __next__(self):
      # If all cells are read, then check if last batch before raising
      # StopIteration
      if self.__next_index >= len(self.cells()):
        if self.__batches[self.__batch_index].is_last_batch:
          raise StopIteration
        self.__batch_index += 1
        self.__next_index = 0
        self.__string_cells = self.__batches[
            self.__batch_index].string_cells.split('\0')

      row = TraceProcessor.Row()
      for num, column_name in enumerate(self.__column_names):
        cell_list = self.get_cell_list(self.cells()[self.__next_index + num])
        if cell_list is not None:
          val = cell_list.pop(0)
          setattr(row, column_name, val)
        else:
          setattr(row, column_name, None)
      self.__next_index = self.__next_index + len(self.__column_names)
      return row

  def __init__(self, addr=None, file_path=None, bin_path=None,
               unique_port=True):
    # Load trace_processor_shell or access via given address
    if addr:
      p = urlparse(addr)
      tp = TraceProcessorHttp(p.netloc if p.netloc else p.path)
    else:
      url, self.subprocess = load_shell(
          bin_path=bin_path, unique_port=unique_port)
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
      can also be converted to a pandas dataframe by calling the as_pandas()
      function after calling query.
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

  # TODO(@aninditaghosh): Investigate context managers for
  # cleaner usage
  def close(self):
    if hasattr(self, 'subprocess'):
      self.subprocess.kill()
    self.http.conn.close()
