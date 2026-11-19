#!/usr/bin/env python3
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
"""Runs pfsql, the C++ tool which parses PerfettoSQL for Python scripts."""

import json
import subprocess
from typing import List


def parse_modules(stdlib_path: str, pfsql_binary: str) -> List[dict]:
  """Returns the modules of `pfsql parse` over a stdlib tree."""
  out = subprocess.check_output([pfsql_binary, 'parse', stdlib_path], text=True)
  return json.loads(out)['modules']
