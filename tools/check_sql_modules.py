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

import argparse
from typing import List, Tuple
import os
import sys
import re

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.docs_parse import ParsedFile, parse_file
from python.generators.sql_processing.utils import check_banned_words


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--stdlib-sources',
      default=os.path.join(ROOT_DIR, "src", "trace_processor", "perfetto_sql",
                           "stdlib"))
  args = parser.parse_args()
  errors = []
  modules: List[Tuple[str, str, ParsedFile]] = []
  for root, _, files in os.walk(args.stdlib_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if not path.endswith(".sql"):
        continue
      with open(path, 'r') as f:
        sql = f.read()

      parsed = parse_file(path, sql)
      modules.append((path, sql, parsed))

  for path, sql, parsed in modules:
    errors += parsed.errors
    errors += check_banned_words(sql, path)

  sys.stderr.write("\n".join(errors))
  sys.stderr.write("\n")
  return 0 if not errors else 1


if __name__ == "__main__":
  sys.exit(main())
