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

from python.generators.stdlib_docs.parse import ParsedFile, parse_file


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

  functions = set()

  for path, sql, parsed in modules:
    errors += parsed.errors

    lines = [l.strip() for l in sql.split('\n')]
    for line in lines:
      # Strip the SQL comments.
      line = re.sub(r'--.*$', '', line)

      # Ban the use of LIKE in non-comment lines.
      if 'like' in line.casefold():
        errors.append('LIKE is banned in trace processor metrics. '
                      'Prefer GLOB instead.')
        errors.append('Offending file: %s' % path)

      # Ban the use of CREATE_FUNCTION.
      if 'create_function' in line.casefold():
        errors.append('CREATE_FUNCTION is deprecated in trace processor. '
                      'Prefer CREATE PERFETTO FUNCTION instead.')
        errors.append('Offending file: %s' % path)

  sys.stderr.write("\n".join(errors))
  sys.stderr.write("\n")
  return 0 if not errors else 1


if __name__ == "__main__":
  sys.exit(main())
