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

import dataclasses as dc
from urllib.parse import urlparse
from typing import List, Optional

from perfetto.common.exceptions import PerfettoException
from perfetto.common.query_result_iterator import QueryResultIterator
from perfetto.trace_processor.http import TraceProcessorHttp
from perfetto.trace_processor.platform import PlatformDelegate
from perfetto.trace_processor.protos import ProtoFactory
from perfetto.trace_processor.shell import load_shell
from perfetto.trace_uri_resolver import registry
from perfetto.trace_uri_resolver.registry import ResolverRegistry

# Defining this field as a module variable means this can be changed by
# implementations at startup and used for all TraceProcessor objects
# without having to specify on each one.
# In Google3, this field is rewritten using Copybara to a implementation
# which can integrates with internal infra.
PLATFORM_DELEGATE = PlatformDelegate

TraceReference = registry.TraceReference

# Custom exception raised if any trace_processor functions return a
# response with an error defined
TraceProcessorException = PerfettoException

@dc.dataclass
class TraceProcessorConfig:
  bin_path: Optional[str]
  unique_port: bool
  verbose: bool
  ingest_ftrace_in_raw: bool
  enable_dev_features: bool
  resolver_registry: Optional[ResolverRegistry]
  load_timeout: int
  extra_flags: Optional[List[str]]

  def __init__(self,
               bin_path: Optional[str] = None,
               unique_port: bool = True,
               verbose: bool = False,
               ingest_ftrace_in_raw: bool = False,
               enable_dev_features=False,
               resolver_registry: Optional[ResolverRegistry] = None,
               load_timeout: int = 2,
               extra_flags: Optional[List[str]] = None):
    self.bin_path = bin_path
    self.unique_port = unique_port
    self.verbose = verbose
    self.ingest_ftrace_in_raw = ingest_ftrace_in_raw
    self.enable_dev_features = enable_dev_features
    self.resolver_registry = resolver_registry
    self.load_timeout = load_timeout
    self.extra_flags = extra_flags


class TraceProcessor:
  QueryResultIterator = QueryResultIterator
  Row = QueryResultIterator.Row

  def __init__(self,
               trace: Optional[TraceReference] = None,
               addr: Optional[str] = None,
               config: TraceProcessorConfig = TraceProcessorConfig(),
               file_path: Optional[str] = None):
    """Create a trace processor instance.

    Args:
      trace: reference to a trace to be loaded into the trace
        processor instance.

        One of several types is supported:
        1) path to a trace file to open and read
        2) a file like object (file, io.BytesIO or similar) to read
        3) a generator yielding bytes
        4) a trace URI which resolves to one of the above types
        5) a trace URI resolver; this is a subclass of
        resolver.TraceUriResolver which generates a reference to a
        trace when the |resolve| method is called on it.

        An URI is similar to a connection string (e.g. for a web
        address or SQL database) which specifies where to lookup traces
        and which traces to pick from this data source. The format of a
        string should be as follows:
        resolver_name:key_1=list,of,values;key_2=value

        Custom resolvers can be provided to handle URIs via
        |config.resolver_registry|.
      addr: address of a running trace processor instance. Useful to query an
        already loaded trace.
      config: configuration options which customize functionality of trace
        processor and the Python binding.
      file_path (deprecated): path to a trace file to load. Use
        |trace| instead of this field: specifying both will cause
        an exception to be thrown.
    """

    if trace and file_path:
      raise TraceProcessorException(
          "trace and file_path cannot both be specified.")

    self.config = config
    self.platform_delegate = PLATFORM_DELEGATE()
    self.protos = ProtoFactory(self.platform_delegate)
    self.resolver_registry = config.resolver_registry or \
      self.platform_delegate.default_resolver_registry()
    self.http = self._create_tp_http(addr)

    if trace or file_path:
      try:
        self._parse_trace(trace if trace else file_path)
      except TraceProcessorException as ex:
        self.close()
        raise ex

  def query(self, sql: str):
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

  def metric(self, metrics: List[str]):
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

  def _create_tp_http(self, addr: str) -> TraceProcessorHttp:
    if addr:
      p = urlparse(addr)
      parsed = p.netloc if p.netloc else p.path
      return TraceProcessorHttp(parsed, protos=self.protos)

    url, self.subprocess = load_shell(self.config.bin_path,
                                      self.config.unique_port,
                                      self.config.verbose,
                                      self.config.ingest_ftrace_in_raw,
                                      self.config.enable_dev_features,
                                      self.platform_delegate,
                                      self.config.load_timeout,
                                      self.config.extra_flags)
    return TraceProcessorHttp(url, protos=self.protos)

  def _parse_trace(self, trace: TraceReference):
    resolved_lst = self.resolver_registry.resolve(trace)
    if not resolved_lst:
      raise TraceProcessorException(
          'trace argument did not resolve to a trace.')

    if len(resolved_lst) > 1:
      raise TraceProcessorException(
          'trace argument resolved to more than one trace. Trace processor '
          'only supports loading a single trace; please use '
          'BatchTraceProcessor to operate on multiple traces.')

    resolved = resolved_lst[0]
    for chunk in resolved.generator:
      result = self.http.parse(chunk)
      if result.error:
        raise TraceProcessorException(
            f'Failed while parsing trace. Error message: {result.error}')
    self.http.notify_eof()

  def __enter__(self):
    return self

  def __exit__(self, a, b, c):
    del a, b, c  # Unused.
    self.close()
    return False

  def close(self):
    if hasattr(self, 'subprocess'):
      self.subprocess.kill()
      self.subprocess.wait()

    if hasattr(self, 'http'):
      self.http.conn.close()

  def __del__(self):
    self.close()
