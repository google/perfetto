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

from urllib import request, error
import subprocess
import tempfile
import time

# URL to download script to run trace_processor
SHELL_URL = 'http://get.perfetto.dev/trace_processor'


def load_shell():
  try:
    from .shell_vendor import load_shell_vendor
    shell_path = load_shell_vendor()
  except ModuleNotFoundError:
    # TODO(@aninditaghosh): Try to use preexisting binary before
    # attempting to download trace_processor
    with tempfile.NamedTemporaryFile(delete=False) as file:
      req = request.Request(SHELL_URL)
      with request.urlopen(req) as req:
        file.write(req.read())
    shell_path = file.name
    subprocess.check_output(['chmod', '+x', shell_path])

  p = subprocess.Popen([shell_path, '-D'], stdout=subprocess.DEVNULL)

  while True:
    try:
      req = request.urlretrieve('http://localhost:9001/status')
      time.sleep(1)
      break
    except error.URLError:
      pass
  return 'localhost:9001', p
