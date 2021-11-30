#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from perfetto.trace_processor import TraceProcessor, TraceProcessorException


@dataclass
class TpArg:
  bin_path: str
  verbose: bool
  file: str


class BatchTraceProcessor:
  """BatchTraceProcessor is the blessed way of running ad-hoc queries on
  Python across many Perfetto traces.

  Usage:
    with BatchTraceProcessor(file_paths=files) as btp:
      dfs = btp.query('select * from slice')
      for df in dfs:
        print(df)
  """

  def __init__(self, file_paths, bin_path=None, verbose=False):
    """Creates a batch trace processor instance: the blessed way of running
    ad-hoc queries on Python across many traces.

    Args:
      file_paths: List of trace file paths to load into this batch trace
        processor instance.
      bin_path: Optional path to a trace processor shell binary to use to
        load the traces.
      verbose: Optional flag indiciating whether verbose trace processor
        output should be printed to stderr.
    """
    self.executor = ThreadPoolExecutor()
    self.paths = file_paths

    def create_tp(arg):
      return TraceProcessor(
          file_path=arg.file, bin_path=arg.bin_path, verbose=arg.verbose)

    tp_args = [TpArg(bin_path, verbose, file) for file in file_paths]
    self.tps = list(self.executor.map(create_tp, tp_args))

  def query(self, sql):
    """Executes the provided SQL statement in parallel across all the traces.

    Args:
      sql: The SQL statement to execute.

    Returns:
      A list of Pandas dataframes with the result of executing the query (one
      per trace).

    Raises:
      TraceProcessorException: An error occurred running the query.
    """
    return self.__execute_on_tps(lambda tp: tp.query(sql).as_pandas_dataframe())

  def query_single_result(self, sql):
    """Executes the provided SQL statement (which should return a single row)
    in parallel across all the traces.

    Args:
      sql: The SQL statement to execute. This statement should return exactly
        one row on any trace.

    Returns:
      A list of values with the result of executing the query (one per ftrace).

    Raises:
      TraceProcessorException: An error occurred running the query or more than
        one result was returned.
    """

    def query_single_result_inner(tp):
      df = tp.query(sql).as_pandas_dataframe()
      if len(df.index) != 1:
        raise TraceProcessorException("Query should only return a single row")

      if len(df.columns) != 1:
        raise TraceProcessorException(
            "Query should only return a single column")

      return df.iloc[0, 0]

    return self.__execute_on_tps(query_single_result_inner)

  def close(self):
    """Closes this batch trace processor instance: this closes all spawned
    trace processor instances, releasing all the memory and resources those
    instances take.

    No further calls to other methods in this class should be made after
    calling this method.
    """
    self.executor.map(lambda tp: tp.close(), self.tps)
    self.executor.shutdown()

  def __execute_on_tps(self, fn):
    return list(self.executor.map(fn, self.tps))

  def __enter__(self):
    return self

  def __exit__(self, _, __, ___):
    self.close()
    return False
