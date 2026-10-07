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

from typing import List
from typing import Optional
from typing import Union
from urllib.parse import urlparse

from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor.http import TraceProcessorHttp
from perfetto.trace_processor.protos import ProtoFactory
from perfetto.trace_processor.unix import TraceProcessorUnix
from perfetto.trace_processor.unix import unix_socket_path_for

# The concrete clients TraceProcessorRemote can route to.
TraceProcessorClient = Union[TraceProcessorHttp, TraceProcessorUnix]


class TraceProcessorRemote:
  """Client for a trace processor server that is already running.

  Picks a concrete client from |remote| using the same rules as
  `trace_processor --remote` and forwards every method to it unchanged, so
  TraceProcessor can use this exactly like TraceProcessorHttp.

  Currently (see unix_socket_path_for()), a Unix socket session name or path
  goes to TraceProcessorUnix, and anything else is treated as an HTTP address
  and goes to TraceProcessorHttp.
  """

  def __init__(self, remote: str, protos: ProtoFactory):
    self._client = self._connect(remote, protos)

  @staticmethod
  def _connect(remote: str, protos: ProtoFactory) -> TraceProcessorClient:
    socket_path = unix_socket_path_for(remote)
    if socket_path:
      try:
        return TraceProcessorUnix(socket_path, protos=protos)
      except (FileNotFoundError, ConnectionRefusedError) as ex:
        # No socket file, or a stale one left behind by a dead server.
        raise PerfettoException(
            f"No live trace processor session '{remote}' at {socket_path}. "
            "Start one with: trace_processor server unix --name <name> "
            "<trace>") from ex

    # Without a scheme (e.g. 'localhost:9123'), urlparse treats the host as
    # the scheme and the port as the path, so we'd connect to the wrong
    # address. Adding an explicit http:// makes parsing unambiguous.
    p = urlparse(remote)
    if p.scheme not in ('http', 'https'):
      p = urlparse('http://' + remote)
    return TraceProcessorHttp(p.netloc, protos=protos)

  def execute_query(self, query: str):
    return self._client.execute_query(query)

  def compute_metric(self, metrics: List[str]):
    return self._client.compute_metric(metrics)

  def trace_summary(self,
                    specs: List[Union[str, bytes]],
                    metric_ids: Optional[List[str]] = None,
                    metadata_query_id: Optional[str] = None):
    return self._client.trace_summary(specs, metric_ids, metadata_query_id)

  def enable_metatrace(self):
    return self._client.enable_metatrace()

  def disable_and_read_metatrace(self):
    return self._client.disable_and_read_metatrace()

  def export(self, output_file, export_format: str):
    return self._client.export(output_file, export_format)

  def parse(self, chunk: bytes):
    return self._client.parse(chunk)

  def notify_eof(self):
    return self._client.notify_eof()

  def close(self):
    self._client.close()
