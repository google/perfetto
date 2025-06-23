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
from typing import List, Optional, Union

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
  # The path to the trace processor binary. If not specified, the trace
  # processor will be automatically downloaded and run from the latest
  # avaialble prebuilts.
  bin_path: Optional[str] = None

  # If True, the trace processor will use a unique port for each instance.
  unique_port: bool = True

  # If True, the trace processor will print verbose output to stdout.
  verbose: bool = False

  # If True, the trace processor will ingest ftrace in the `ftrace_event`
  # table.
  ingest_ftrace_in_raw: bool = False

  # If True, the trace processor will enable development features.
  # Any feature gated behind this flag is not guaranteed to be stable
  # and may change or be removed in future versions.
  # This flag is intended for use by developers and testers.
  enable_dev_features: bool = False

  # A registry of custom URI resolvers to use when resolving trace URIs.
  resolver_registry: Optional[ResolverRegistry] = None

  # The timeout in seconds for the trace processor binary starting up. If the
  # binary does not start within this time, an exception will be raised.
  load_timeout: int = 2

  # Any extra flags to pass to the trace processor binary.
  # Warning: this is a low-level option and should be used with caution.
  extra_flags: Optional[List[str]] = None

  # Optional list of paths to additional PerfettoSQL package to load.
  # All SQL modules inside these packages will be available to include using
  # `INCLUDE PERFETTO MODULE` PerfettoSQL statements with the root package
  # name being the dirname of the path.
  add_sql_packages: Optional[List[str]] = None

  def __init__(
      self,
      bin_path: Optional[str] = None,
      unique_port: bool = True,
      verbose: bool = False,
      ingest_ftrace_in_raw: bool = False,
      enable_dev_features=False,
      resolver_registry: Optional[ResolverRegistry] = None,
      load_timeout: int = 2,
      extra_flags: Optional[List[str]] = None,
      add_sql_packages: Optional[List[str]] = None,
  ):
    self.bin_path = bin_path
    self.unique_port = unique_port
    self.verbose = verbose
    self.ingest_ftrace_in_raw = ingest_ftrace_in_raw
    self.enable_dev_features = enable_dev_features
    self.resolver_registry = resolver_registry
    self.load_timeout = load_timeout
    self.extra_flags = extra_flags
    self.add_sql_packages = add_sql_packages


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

  def trace_summary(self,
                    specs: List[Union[str, bytes]],
                    metric_ids: Optional[List[str]] = None,
                    metadata_query_id: Optional[str] = None):
    """Computes a structuted summary of the trace.

    This function allows you to create a structured summary of the trace
    data output in as a structured protobuf message, allowing consumption by
    other tools for charting or bulk analysis.

    This function is the replacement for the `metric` function, which is now
    deprecated (but without any plans to remove it).

    Raises TraceProcessorException if there was an error computing the summary.

    Args:
      specs: a list of `TraceSummarySpec` protos either as a textproto or in
        binary format. Please see the definition of the `TraceSummarySpec`
        proto for more details on how to construct these.
      metric_ids: Optional list of metric IDs to compute in the summary.
        If `None`, all metrics in the specs will be computed.
      metadata_query_id: Optional query ID for metadata

    Returns:
      The trace summary data as a proto message
    """
    response = self.http.trace_summary(specs, metric_ids, metadata_query_id)
    if response.error:
      raise TraceProcessorException(response.error)

    summary = self.protos.TraceSummary()
    summary.ParseFromString(response.proto_summary)
    return summary

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

  def metric(self, metrics: List[str]):
    """Returns the metrics data corresponding to the passed in trace metric.
    Raises TraceProcessorException if the response returns with an error.

    Note: this function is deprecated but there are no plans to remove it.
    Consider using `trace_summary` instead, which is an indirect replacement,
    providing much of the same functionality but in a more flexible way.

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

  def _create_tp_http(self, addr: str) -> TraceProcessorHttp:
    if addr:
      p = urlparse(addr)
      parsed = p.netloc if p.netloc else p.path
      return TraceProcessorHttp(parsed, protos=self.protos)

    url, self.subprocess = load_shell(
        self.config.bin_path,
        self.config.unique_port,
        self.config.verbose,
        self.config.ingest_ftrace_in_raw,
        self.config.enable_dev_features,
        self.platform_delegate,
        self.config.load_timeout,
        self.config.extra_flags,
        self.config.add_sql_packages,
    )
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
