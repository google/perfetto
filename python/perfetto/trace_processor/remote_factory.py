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

from urllib.parse import urlparse

from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor.http import TraceProcessorHttp
from perfetto.trace_processor.protos import ProtoFactory
from perfetto.trace_processor.remote import TraceProcessorRemote
from perfetto.trace_processor.unix import TraceProcessorUnix
from perfetto.trace_processor.unix import unix_socket_path_for


def create_remote(remote: str, protos: ProtoFactory) -> TraceProcessorRemote:
  """Connects to the already running trace processor instance |remote|.

  Uses the same rules as `trace_processor --remote` (see
  unix_socket_path_for()): a Unix socket session name or path connects over
  the Unix socket, and anything else is treated as an HTTP address.
  """
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
