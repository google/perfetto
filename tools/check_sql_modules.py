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
# '_' is documented with proper schema.

import argparse
from typing import List, Tuple
import os
import sys
import re

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.docs_parse import ParsedFile
from python.generators.sql_processing.docs_parse import parse_file
from python.generators.sql_processing.utils import check_banned_create_table_as
from python.generators.sql_processing.utils import check_banned_create_view_as
from python.generators.sql_processing.utils import check_banned_words
from python.generators.sql_processing.utils import check_banned_include_all

# Allowlist path are relative to the stdlib root.
CREATE_TABLE_ALLOWLIST = {
    '/prelude/trace_bounds.sql': ['trace_bounds'],
    '/android/binder.sql': ['_oom_score'],
    '/android/monitor_contention.sql': [
        '_isolated', 'android_monitor_contention_chain',
        'android_monitor_contention'
    ],
    '/chrome/tasks.sql': [
        '_chrome_mojo_slices', '_chrome_java_views', '_chrome_scheduler_tasks',
        '_chrome_tasks'
    ],
    '/sched/thread_executing_span.sql': [
        '_wakeup', '_thread_executing_span_graph', '_critical_path',
        '_wakeup_graph', '_thread_executing_span_graph'
    ],
    '/slices/flat_slices.sql': ['_slice_flattened']
}


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--stdlib-sources',
      default=os.path.join(ROOT_DIR, "src", "trace_processor", "perfetto_sql",
                           "stdlib"))
  parser.add_argument(
      '--verbose',
      action='store_true',
      default=False,
      help='Enable additional logging')
  parser.add_argument(
      '--name-filter',
      default=None,
      type=str,
      help='Filter the name of the modules to check (regex syntax)')

  args = parser.parse_args()
  errors = []
  modules: List[Tuple[str, str, ParsedFile]] = []
  for root, _, files in os.walk(args.stdlib_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if not path.endswith(".sql"):
        continue
      rel_path = os.path.relpath(path, args.stdlib_sources)
      if args.name_filter is not None:
        pattern = re.compile(args.name_filter)
        if not pattern.match(rel_path):
          continue

      if args.verbose:
        print(f'Parsing {rel_path}:')

      with open(path, 'r') as f:
        sql = f.read()

      parsed = parse_file(rel_path, sql)

      # Some modules (i.e. `deprecated`) should not be checked.
      if not parsed:
        continue

      modules.append((path, sql, parsed))

      if args.verbose:
        function_count = len(parsed.functions) + len(parsed.table_functions)
        print(f'Parsed {function_count} functions'
              f', {len(parsed.table_views)} tables/views'
              f' ({len(parsed.errors)} errors).')

  for path, sql, parsed in modules:
    lines = [l.strip() for l in sql.split('\n')]
    for line in lines:
      if line.startswith('--'):
        continue
      if 'RUN_METRIC' in line:
        errors.append(f"RUN_METRIC is banned in standard library.\n"
                      f"Offending file: {path}\n")
      if 'insert into' in line.casefold():
        errors.append(f"INSERT INTO table is not allowed in standard library.\n"
                      f"Offending file: {path}\n")

    errors += parsed.errors
    errors += check_banned_words(sql, path)
    errors += check_banned_create_table_as(
        sql,
        path.split(ROOT_DIR)[1],
        args.stdlib_sources.split(ROOT_DIR)[1], CREATE_TABLE_ALLOWLIST)
    errors += check_banned_create_view_as(sql, path.split(ROOT_DIR)[1])
    errors += check_banned_include_all(sql, path.split(ROOT_DIR)[1])

  if errors:
    sys.stderr.write("\n".join(errors))
    sys.stderr.write("\n")
  return 0 if not errors else 1


if __name__ == "__main__":
  sys.exit(main())
