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
import abc
import concurrent.futures as cf
import dataclasses as dc
import time
from enum import Enum
import multiprocessing
from typing import Any, Callable, Dict, Tuple, List, Optional

import pandas as pd

from perfetto.batch_trace_processor.platform import PlatformDelegate
from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor.api import PLATFORM_DELEGATE as TP_PLATFORM_DELEGATE
from perfetto.trace_processor.api import TraceProcessor
from perfetto.trace_processor.api import TraceProcessorException
from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.trace_uri_resolver import registry
from perfetto.trace_uri_resolver.registry import ResolverRegistry

# Defining this field as a module variable means this can be changed by
# implementations at startup and used for all BatchTraceProcessor objects
# without having to specify on each one.
# In Google3, this field is rewritten using Copybara to a implementation
# which can integrates with internal infra.
PLATFORM_DELEGATE = PlatformDelegate

TraceListReference = registry.TraceListReference
Metadata = Dict[str, str]

MAX_LOAD_WORKERS = 32


# Enum encoding how errors while loading/querying traces in BatchTraceProcessor
# should be handled.
class FailureHandling(Enum):
  # If any trace fails to load or be queried, raises an exception causing the
  # entire batch trace processor to fail.
  # This is the default behaviour and the method which should be preferred for
  # any interactive use of BatchTraceProcessor.
  RAISE_EXCEPTION = 0

  # If a trace fails to load or be queried, the trace processor for that trace
  # is dropped but loading of other traces is unaffected. A failure integer is
  # incremented in the Stats class for the batch trace processor instance.
  INCREMENT_STAT = 1


@dc.dataclass
class BatchTraceProcessorConfig:
  tp_config: TraceProcessorConfig
  load_failure_handling: FailureHandling
  execute_failure_handling: FailureHandling

  def __init__(
      self,
      tp_config: TraceProcessorConfig = TraceProcessorConfig(),
      load_failure_handling: FailureHandling = FailureHandling.RAISE_EXCEPTION,
      execute_failure_handling: FailureHandling = FailureHandling
      .RAISE_EXCEPTION,
  ):
    self.tp_config = tp_config
    self.load_failure_handling = load_failure_handling
    self.execute_failure_handling = execute_failure_handling


# Contains stats about the events which happened during the use of
# BatchTraceProcessor.
@dc.dataclass
class Stats:
  # The number of traces which failed to load; only non-zero if
  # FailureHanding.INCREMENT_STAT is chosen as the load failure handling type.
  load_failures: int = 0

  # The number of traces which failed while executing (query, metric or
  # arbitary function); only non-zero if FailureHanding.INCREMENT_STAT is
  # chosen as the execute failure handling type.
  execute_failures: int = 0


class BatchTraceProcessor:
  """Run ad-hoc SQL queries across many Perfetto traces.

  Usage:
    with BatchTraceProcessor(traces) as btp:
      dfs = btp.query('select * from slice')
      for df in dfs:
        print(df)
  """

  class Observer(abc.ABC):
    """Observer that can be used to provide side-channel information about
    processed traces.
    """

    @abc.abstractmethod
    def trace_processed(self, metadata: Metadata,
                        execution_time_seconds: float):
      """Invoked every time query has been executed on a trace.

      Args:
        metadata: Metadata provided by trace resolver, can be used to identify
          the trace.

        execution_time_seconds: Query execution time, in seconds.
      """
      raise NotImplementedError

  def __init__(self,
               traces: TraceListReference,
               config: BatchTraceProcessorConfig = BatchTraceProcessorConfig(),
               observer: Optional[Observer] = None):
    """Creates a batch trace processor instance.

    BatchTraceProcessor is the blessed way of running ad-hoc queries in
    Python across many traces.

    Args:
      traces: A list of traces, a trace URI resolver or a URI which
        can be resolved to a list of traces.

        If a list, each of items must be one of the following types:
        1) path to a trace file to open and read
        2) a file like object (file, io.BytesIO or similar) to read
        3) a generator yielding bytes
        4) an URI which resolves to a trace

        A trace URI resolver is a subclass of resolver.TraceUriResolver
        which generates a list of trace references when the |resolve|
        method is called on it.

        A URI is similar to a connection string (e.g. for a web
        address or SQL database) which specifies where to lookup traces
        and which traces to pick from this data source. The format of a
        string should be as follows:
        resolver_name:key_1=list,of,values;key_2=value

        Custom resolvers can be provided to handle URIs via
        |config.resolver_registry|.
      config: configuration options which customize functionality of batch
        trace processor and underlying trace processors.
      observer: an optional observer for side-channel information, e.g.
        running time of queries.
    """

    self.tps_and_metadata = None
    self.closed = False
    self._stats = Stats()

    self.platform_delegate = PLATFORM_DELEGATE()
    self.tp_platform_delegate = TP_PLATFORM_DELEGATE()
    self.config = config

    self.observer = observer

    # Make sure the descendent trace processors are using the same resolver
    # registry (even though they won't actually use it as we will resolve
    # everything fully in this class).
    self.resolver_registry = config.tp_config.resolver_registry or \
      self.tp_platform_delegate.default_resolver_registry()
    self.config.tp_config.resolver_registry = self.resolver_registry

    # Resolve all the traces to their final form.
    resolved = self.resolver_registry.resolve(traces)

    # As trace processor is completely CPU bound, it makes sense to just
    # max out the CPUs available.
    query_executor = self.platform_delegate.create_query_executor(
        len(resolved)) or cf.ThreadPoolExecutor(
            max_workers=multiprocessing.cpu_count())
    # Loading trace involves FS access, so it makes sense to limit parallelism
    max_load_workers = min(multiprocessing.cpu_count(), MAX_LOAD_WORKERS)
    load_executor = self.platform_delegate.create_load_executor(
        len(resolved)) or cf.ThreadPoolExecutor(max_workers=max_load_workers)

    self.query_executor = query_executor
    self.tps_and_metadata = [
        x for x in load_executor.map(self._create_tp, resolved) if x is not None
    ]

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

      If an URI or a trace resolver was passed to the constructor, the
      contents of the |metadata| dictionary emitted by the resolver will also
      be emitted as extra columns (key being column name, value being the
      value in the dataframe).

      For example:
        class CustomResolver(TraceUriResolver):
          def resolve(self):
            return [TraceUriResolver.Result(trace='/tmp/path',
                                            metadata={
                                              'path': '/tmp/path'
                                              'foo': 'bar'
                                            })]

        with BatchTraceProcessor(CustomResolver()) as btp:
          df = btp.query_and_flatten('select count(1) as cnt from slice')

      Then df will look like this:
        cnt       path              foo
        100       /tmp/path         bar

    Raises:
      TraceProcessorException: An error occurred running the query.
    """
    return self.execute_and_flatten(
        lambda tp: tp.query(sql).as_pandas_dataframe())

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

    def wrapped(pair: Tuple[TraceProcessor, Metadata]):
      (tp, metadata) = pair
      return self._execute_handling_failure(fn, tp, metadata)

    return list(self.query_executor.map(wrapped, self.tps_and_metadata))

  def execute_and_flatten(
      self, fn: Callable[[TraceProcessor], pd.DataFrame]) -> pd.DataFrame:
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

    def wrapped(pair: Tuple[TraceProcessor, Metadata]):
      (tp, metadata) = pair
      start = time.time()
      df = self._execute_handling_failure(fn, tp, metadata)
      end = time.time()
      if self.observer:
        self.observer.trace_processed(metadata, end - start)
      for key, value in metadata.items():
        df[key] = value
      return df

    df = pd.concat(
        list(self.query_executor.map(wrapped, self.tps_and_metadata)))
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

    if self.tps_and_metadata:
      for tp, _ in self.tps_and_metadata:
        tp.close()

  def stats(self):
    """Statistics about the operation of this batch trace processor instance.
    
    See |Stats| class definition for the list of the statistics available."""
    return self._stats

  def _create_tp(
      self, trace: ResolverRegistry.Result
  ) -> Optional[Tuple[TraceProcessor, Metadata]]:
    try:
      return TraceProcessor(
          trace=trace.generator, config=self.config.tp_config), trace.metadata
    except Exception as ex:
      if self.config.load_failure_handling == FailureHandling.RAISE_EXCEPTION:
        raise ex
      self._stats.load_failures += 1
      return None

  def _execute_handling_failure(self, fn: Callable[[TraceProcessor], Any],
                                tp: TraceProcessor, metadata: Metadata):
    try:
      return fn(tp)
    except TraceProcessorException as ex:
      if self.config.execute_failure_handling == \
          FailureHandling.RAISE_EXCEPTION:
        raise TraceProcessorException(f'{metadata} {ex}') from None
      self._stats.execute_failures += 1
      return pd.DataFrame()

  def __enter__(self):
    return self

  def __exit__(self, a, b, c):
    del a, b, c  # Unused.
    self.close()
    return False

  def __del__(self):
    self.close()
