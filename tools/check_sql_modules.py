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
from python.generators.sql_processing.utils import check_banned_create_table_as

CREATE_TABLE_ALLOWLIST = {
    '/src/trace_processor/perfetto_sql/stdlib/android/binder.sql': [
        'internal_oom_score', 'internal_async_binder_reply',
        'internal_binder_async_txn_raw'
    ],
    '/src/trace_processor/perfetto_sql/stdlib/android/monitor_contention.sql': [
        'internal_isolated', 'android_monitor_contention_chain',
        'android_monitor_contention'
    ],
    '/src/trace_processor/perfetto_sql/stdlib/chrome/tasks.sql': [
        'internal_chrome_mojo_slices', 'internal_chrome_java_views',
        'internal_chrome_scheduler_tasks', 'internal_chrome_tasks'
    ],
    ('/src/trace_processor/perfetto_sql/stdlib/experimental/'
     'thread_executing_span.sql'): [
        'internal_wakeup', 'experimental_thread_executing_span_graph',
        'internal_critical_path', 'internal_wakeup_graph', 'experimental_thread_executing_span_graph'
    ],
    '/src/trace_processor/perfetto_sql/stdlib/experimental/flat_slices.sql': [
        'experimental_slice_flattened'
    ]
}


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
    lines = [l.strip() for l in sql.split('\n')]
    for line in lines:
      if line.startswith('--'):
        continue
      if 'RUN_METRIC' in line:
        errors.append(f"RUN_METRIC is banned in standard library.\n"
                      f"Offending file: {path}\n")

    errors += parsed.errors
    errors += check_banned_words(sql, path)
    errors += check_banned_create_table_as(sql,
                                           path.split(ROOT_DIR)[1],
                                           CREATE_TABLE_ALLOWLIST)

  if errors:
    sys.stderr.write("\n".join(errors))
    sys.stderr.write("\n")
  return 0 if not errors else 1


if __name__ == "__main__":
  sys.exit(main())
