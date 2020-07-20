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

# Limit parsing file to 32MB to maintain parity with the UI
MAX_BYTES_LOADED = 32 * 1024 * 1024


def parse_file(tp_http, file_path):
  try:
    from .parse_vendor import parse_file_vendor
    return parse_file_vendor(tp_http, file_path)
  except ModuleNotFoundError:
    with open(file_path, 'rb') as f:
      f_size = os.path.getsize(file_path)
      bytes_read = 0
      while (bytes_read < f_size):
        chunk = f.read(MAX_BYTES_LOADED)
        tp_http.parse(chunk)
        bytes_read += len(chunk)
    tp_http.notify_eof()
    return tp_http
