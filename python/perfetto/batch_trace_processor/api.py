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
"""Contains classes for BatchTraceProcessor API."""

import concurrent.futures as cf
import dataclasses as dc
import multiprocessing
from typing import Any, Callable, Dict, Tuple, Union, List

import pandas as pd

from perfetto.trace_processor.api import LoadableTrace
from perfetto.trace_processor.api import TraceProcessor
from perfetto.trace_processor.api import TraceProcessorException
from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.batch_trace_processor.platform import PlatformDelegate

# Defining this field as a module variable means this can be changed by
# implementations at startup and used for all BatchTraceProcessor objects
# without having to specify on each one.
# In Google3, this field is rewritten using Copybara to a implementation
# which can integrates with internal infra.
PLATFORM_DELEGATE = PlatformDelegate


@dc.dataclass
class BatchLoadableTrace:
  trace: LoadableTrace
  args: Dict[str, str]


@dc.dataclass
class BatchTraceProcessorConfig:
  tp_config: TraceProcessorConfig

  def __init__(self, tp_config: TraceProcessorConfig = TraceProcessorConfig()):
    self.tp_config = tp_config


class BatchTraceProcessor:
  """Run ad-hoc SQL queries across many Perfetto traces.

  Usage:
    with BatchTraceProcessor(traces) as btp:
      dfs = btp.query('select * from slice')
      for df in dfs:
        print(df)
  """

  def __init__(
      self,
      traces: Union[str, List[Union[LoadableTrace, BatchLoadableTrace]]],
      config: BatchTraceProcessorConfig = BatchTraceProcessorConfig()):
    """Creates a batch trace processor instance.

    BatchTraceProcessor is the blessed way of running ad-hoc queries in
    Python across many traces.

    Args:
      traces: A list of traces where each item is one of the following types:
        1) path to a trace file to open and read
        2) a file like object (file, io.BytesIO or similar) to read
        3) a generator yielding bytes
        4) a BatchLoadableTrace object; this is basically a wrapper around
           one of the above types plus an args field; see |query_and_flatten|
           for the motivation for the args field.
      config: configuration options which customize functionality of batch
        trace processor and underlying trace processors.
    """

    def _create_batch_trace(x: Union[LoadableTrace, BatchLoadableTrace]
                           ) -> BatchLoadableTrace:
      if isinstance(x, BatchLoadableTrace):
        return x
      return BatchLoadableTrace(trace=x, args={})

    def _create_tp(trace: BatchLoadableTrace) -> TraceProcessor:
      return TraceProcessor(trace=trace.trace, config=config.tp_config)

    batch_traces = [_create_batch_trace(t) for t in traces]
    trace_count = len(batch_traces)

    self.platform_delegate = PLATFORM_DELEGATE()

    # As trace processor is completely CPU bound, it makes sense to just
    # max out the CPUs available.
    query_executor = self.platform_delegate.create_query_executor(
        trace_count) or cf.ThreadPoolExecutor(
            max_workers=multiprocessing.cpu_count())
    load_exectuor = self.platform_delegate.create_load_executor(
        trace_count) or query_executor

    self.tps = None
    self.closed = False
    self.query_executor = query_executor
    self.args = [t.args for t in batch_traces]
    self.tps = list(load_exectuor.map(_create_tp, batch_traces))

  def metric(self, metrics: List[str]):
    """Computes the provided metrics.

    The computation happens in parallel across all the traces.

    Args:
      metrics: A list of valid metrics as defined in TraceMetrics

    Returns:
      A list of TraceMetric protos (one for each trace).
    """
    return self.execute(lambda tp: tp.metric(metrics))

  def query(self, sql: str):
    """Executes the provided SQL statement (returning a single row).

    The execution happens in parallel across all the traces.

    Args:
      sql: The SQL statement to execute.

    Returns:
      A list of Pandas dataframes with the result of executing the query (one
      per trace).

    Raises:
      TraceProcessorException: An error occurred running the query.
    """
    return self.execute(lambda tp: tp.query(sql).as_pandas_dataframe())

  def query_and_flatten(self, sql: str):
    """Executes the provided SQL statement and flattens the result.

    The execution happens in parallel across all the traces and the
    resulting Pandas dataframes are flattened into a single dataframe.

    Args:
      sql: The SQL statement to execute.

    Returns:
      A concatenated Pandas dataframe containing the result of executing the
      query across all the traces.

      If |BatchLoadableTrace| objects were passed to the constructor, the
      contents of the |args| dictionary will also be emitted as extra columns
      (key being column name, value being the value in the dataframe).

      For example:
        traces = [BatchLoadableTrace(trace='/tmp/path', args={"foo": "bar"})]
        with BatchTraceProcessor(traces) as btp:
          df = btp.query_and_flatten('select count(1) as cnt from slice')

      Then df will look like this:
        cnt             foo
        100             bar

    Raises:
      TraceProcessorException: An error occurred running the query.
    """
    return self.execute_and_flatten(lambda tp: tp.query(sql).
                                    as_pandas_dataframe())

  def query_single_result(self, sql: str):
    """Executes the provided SQL statement (returning a single row).

    The execution happens in parallel across all the traces.

    Args:
      sql: The SQL statement to execute. This statement should return exactly
        one row on any trace.

    Returns:
      A list of values with the result of executing the query (one per trace).

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

    return self.execute(query_single_result_inner)

  def execute(self, fn: Callable[[TraceProcessor], Any]) -> List[Any]:
    """Executes the provided function.

    The execution happens in parallel across all the trace processor instances
    owned by this object.

    Args:
      fn: The function to execute.

    Returns:
      A list of values with the result of executing the fucntion (one per
      trace).
    """
    return list(self.query_executor.map(fn, self.tps))

  def execute_and_flatten(self, fn: Callable[[TraceProcessor], pd.DataFrame]
                         ) -> pd.DataFrame:
    """Executes the provided function and flattens the result.

    The execution happens in parallel across all the trace processor
    instances owned by this object and the returned Pandas dataframes are
    flattened into a single dataframe.

    Args:
      fn: The function to execute which returns a Pandas dataframe.

    Returns:
      A Pandas dataframe containing the result of executing the query across all
      the traces. Extra columns containing the file path and args will
      be added to the dataframe (see |query_and_flatten| for details).
    """

    def wrapped(pair: Tuple[TraceProcessor, BatchLoadableTrace]):
      (tp, args) = pair
      df = fn(tp)
      for key, value in args.items():
        df[key] = value
      return df

    df = pd.concat(
        list(self.query_executor.map(wrapped, zip(self.tps, self.args))))
    return df.reset_index(drop=True)

  def close(self):
    """Closes this batch trace processor instance.

    This closes all spawned trace processor instances, releasing all the memory
    and resources those instances take.

    No further calls to other methods in this class should be made after
    calling this method.
    """
    if self.closed:
      return
    self.closed = True

    if self.tps:
      for tp in self.tps:
        tp.close()

  def __enter__(self):
    return self

  def __exit__(self, a, b, c):
    del a, b, c  # Unused.
    self.close()
    return False

  def __del__(self):
    self.close()
