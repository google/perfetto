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
import subprocess
import time
from urllib import request, error

from .loader import get_loader


def load_shell(bin_path=None, unique_port=False):
  shell_path = get_loader().get_shell_path(bin_path=bin_path)
  port, url = get_loader().get_free_port(unique_port=unique_port)
  p = subprocess.Popen([shell_path, '-D', '--http-port', port],
                       stdout=subprocess.DEVNULL)

  while True:
    try:
      if p.poll() != None:
        if unique_port:
          raise Exception(
              "Random port allocation failed, please file a bug at https://goto.google.com/perfetto-bug"
          )
        raise Exception(
            "Trace processor failed to start, please file a bug at https://goto.google.com/perfetto-bug"
        )
      req = request.urlretrieve(f'http://{url}/status')
      time.sleep(1)
      break
    except error.URLError:
      pass

  return url, p
