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

import os
import socket
import subprocess
import tempfile
from typing import Tuple
from urllib import request

from perfetto.trace_uri_resolver.path import PathUriResolver
from perfetto.trace_uri_resolver.registry import ResolverRegistry

# URL to download script to run trace_processor
SHELL_URL = 'http://get.perfetto.dev/trace_processor'


class PlatformDelegate:
  """Abstracts operations which can vary based on platform."""

  def get_resource(self, file: str) -> bytes:
    ws = os.path.dirname(__file__)
    with open(os.path.join(ws, file), 'rb') as x:
      return x.read()

  def get_shell_path(self, bin_path: str) -> str:
    if bin_path is not None:
      if not os.path.isfile(bin_path):
        raise Exception('Path to binary is not valid')
      return bin_path

    with tempfile.NamedTemporaryFile(delete=False) as file:
      req = request.Request(SHELL_URL)
      with request.urlopen(req) as req:
        file.write(req.read())
    if os.name != 'nt':
      subprocess.check_output(['chmod', '+x', file.name])
    return file.name

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
