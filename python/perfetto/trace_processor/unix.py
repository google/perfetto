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

import os
import re
import socket
import tempfile
from typing import Optional

from perfetto.trace_processor.protos import ProtoFactory
from perfetto.trace_processor.rpc import TraceProcessorRpc

# Session names must match ^[A-Za-z0-9][A-Za-z0-9_-]*$ and be at most 64 chars,
# so they are safe to embed in a path and can't be confused with a host:port.
_SESSION_NAME_RE = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]*')
_MAX_SESSION_NAME_LEN = 64


def _is_valid_session_name(name: str) -> bool:
  """Mirrors IsValidSessionName() in src/trace_processor/rpc/session_paths.cc."""
  return len(name) <= _MAX_SESSION_NAME_LEN and bool(
      _SESSION_NAME_RE.fullmatch(name))


def unix_socket_path_for(remote: str) -> Optional[str]:
  """Resolves `remote` to a Unix socket path, or None if it isn't one.

  Mirrors src/trace_processor/rpc/session_paths.cc so an address means the
  same thing here and for `trace_processor --remote`:
    1. ends in ".sock" or is an absolute path -> used as-is
       (ClassifyRemoteAddr()).
    2. a valid session name -> <session dir>/<name>.sock, where <session dir>
       is $XDG_RUNTIME_DIR/perfetto, falling back to the system temp dir
       (SessionSocketPath()).
  Anything else (e.g. "host:port", "http://...") is not a Unix socket.
  """
  if remote.endswith('.sock') or os.path.isabs(remote):
    return remote
  if _is_valid_session_name(remote):
    base_dir = os.environ.get('XDG_RUNTIME_DIR') or tempfile.gettempdir()
    return os.path.join(base_dir, 'perfetto', remote + '.sock')
  return None


class UnixTransport:
  """Moves raw bytes over an AF_UNIX stream socket.

  Knows nothing about protos or framing: that's TraceProcessorRpc's job. Any
  object with the same send()/recv()/close() methods can replace this one.
  """

  def __init__(self, path: str):
    self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
      self.sock.connect(path)
    except OSError:
      self.sock.close()
      raise

  def send(self, data: bytes):
    self.sock.sendall(data)

  def recv(self, max_bytes: int) -> bytes:
    return self.sock.recv(max_bytes)

  def close(self):
    self.sock.close()


class TraceProcessorUnix(TraceProcessorRpc):
  """Client for a trace processor session served over a Unix socket.

  The counterpart of TraceProcessorHttp for sessions started with
  `trace_processor server unix`. `path` is the socket path; see
  unix_socket_path_for() to resolve one from a session name.
  """

  def __init__(self, path: str, protos: ProtoFactory):
    super().__init__(UnixTransport(path), protos)
