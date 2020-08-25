#!/usr/bin/env python3
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

import os
import socket
import subprocess
import tempfile
from urllib import request


# This class contains all functions that first try to use a vendor to fulfil
# their function
class LoaderStandalone:
  # Limit parsing file to 32MB to maintain parity with the UI
  MAX_BYTES_LOADED = 32 * 1024 * 1024

  # URL to download script to run trace_processor
  SHELL_URL = 'http://get.perfetto.dev/trace_processor'

  # Default port that trace_processor_shell runs on
  TP_PORT = '9001'

  def read_tp_descriptor():
    ws = os.path.dirname(__file__)
    with open(os.path.join(ws, 'trace_processor.descriptor'), 'rb') as x:
      return x.read()

  def read_metrics_descriptor():
    ws = os.path.dirname(__file__)
    with open(os.path.join(ws, 'metrics.descriptor'), 'rb') as x:
      return x.read()

  def parse_file(tp_http, file_path):
    with open(file_path, 'rb') as f:
      f_size = os.path.getsize(file_path)
      bytes_read = 0
      while (bytes_read < f_size):
        chunk = f.read(LoaderStandalone.MAX_BYTES_LOADED)
        tp_http.parse(chunk)
        bytes_read += len(chunk)
    tp_http.notify_eof()
    return tp_http

  def get_shell_path(bin_path=None):
    # Try to use preexisting binary before attempting to download
    # trace_processor
    if bin_path is None:
      with tempfile.NamedTemporaryFile(delete=False) as file:
        req = request.Request(LoaderStandalone.SHELL_URL)
        with request.urlopen(req) as req:
          file.write(req.read())
      subprocess.check_output(['chmod', '+x', file.name])
      return file.name
    else:
      if not os.path.isfile(bin_path):
        raise Exception('Path to binary is not valid')
      return bin_path

  def get_free_port(unique_port=False):
    if not unique_port:
      return LoaderStandalone.TP_PORT, f'localhost:{LoaderStandalone.TP_PORT}'
    free_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    free_socket.bind(('', 0))
    free_socket.listen(5)
    port = free_socket.getsockname()[1]
    free_socket.close()
    return str(port), f"localhost:{str(port)}"


# Return vendor class if it exists before falling back on LoaderStandalone
def get_loader():
  try:
    from .loader_vendor import LoaderVendor
    return LoaderVendor
  except ModuleNotFoundError:
    return LoaderStandalone
