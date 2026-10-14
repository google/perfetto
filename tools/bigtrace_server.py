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
"""Local Bigtrace: serve traces, query many at once. See --help."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), os.pardir, "python"))

if __name__ == "__main__":
  from perfetto.bigtrace.cli import main
  sys.exit(main())
