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

# This tool checks that every SQL object created without prefix
# 'internal_' is documented with proper schema.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))
FILE_DIR = ROOT_DIR

from python.generators.stdlib_docs.stdlib import *


def main():

  errors = []
  metrics_sources = os.path.join(FILE_DIR, "src", "trace_processor", "stdlib")
  for root, _, files in os.walk(metrics_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if path.endswith(".sql"):
        with open(path) as f:
          sql = f.read()
        errors += parse_file_to_dict(path, sql)[1]
  sys.stderr.write("\n\n".join(errors))
  return 0 if not errors else 1


if __name__ == "__main__":
  sys.exit(main())
