#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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

import contextlib
import datetime
from datetime import timezone
import os
import socket
import stat
import tempfile
from typing import Optional, Tuple
from urllib import request

from perfetto.trace_uri_resolver.path import PathUriResolver
from perfetto.trace_uri_resolver.registry import ResolverRegistry

# URL to download script to run trace_processor
SHELL_URL = 'https://get.perfetto.dev/trace_processor'


class PlatformDelegate:
  """Abstracts operations which can vary based on platform."""

  def get_resource(self, file: str) -> bytes:
    ws = os.path.dirname(__file__)
    with open(os.path.join(ws, file), 'rb') as x:
      return x.read()

  def get_shell_path(self, bin_path: Optional[str]) -> str:
    if bin_path is not None:
      if not os.path.isfile(bin_path):
        raise Exception(f'Path to binary is not valid ({bin_path}).')
      return bin_path

    tp_path = os.path.join(tempfile.gettempdir(), 'trace_processor_python_api')
    if self._should_download_tp(tp_path):
      with contextlib.ExitStack() as stack:
        req = stack.enter_context(request.urlopen(request.Request(SHELL_URL)))
        file = stack.enter_context(open(tp_path, 'wb'))
        file.write(req.read())
    st = os.stat(tp_path)
    os.chmod(tp_path, st.st_mode | stat.S_IEXEC)
    return tp_path

  def _should_download_tp(self, tp_path):
    try:
      st = os.stat(tp_path)

      # If the file was empty (i.e. failed to be written properly last time),
      # download it.
      if st.st_size == 0:
        return True

      # Try and redownload if we last modified this file more than 7 days
      # ago.
      mod_time = datetime.datetime.fromtimestamp(st.st_mtime, tz=timezone.utc)
      cutoff = datetime.datetime.now().astimezone() - datetime.timedelta(days=7)
      return mod_time < cutoff
    except OSError:
      # Should happen if the file does not exist (i.e. this function has not
      # been run before or tmp was cleared).
      return True

  def get_bind_addr(self, port: int) -> Tuple[str, int]:
    if port:
      return 'localhost', port

    free_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    free_socket.bind(('', 0))
    free_socket.listen(5)
    port = free_socket.getsockname()[1]
    free_socket.close()
    return 'localhost', port

  def default_resolver_registry(self) -> ResolverRegistry:
    return ResolverRegistry(resolvers=[PathUriResolver])
