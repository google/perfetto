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


def load_shell(bin_path=None):
  shell_path = get_loader().get_shell_path(bin_path=bin_path)
  p = subprocess.Popen([shell_path, '-D'], stdout=subprocess.DEVNULL)

  while True:
    try:
      req = request.urlretrieve('http://localhost:9001/status')
      time.sleep(1)
      break
    except error.URLError:
      pass
  return 'localhost:9001', p
