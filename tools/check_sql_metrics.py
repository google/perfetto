#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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

# This tool checks that every create (table|view) is prefixed by
# drop (table|view).

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import sys
from typing import Dict, Tuple, List

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.utils import match_pattern
from python.generators.sql_processing.utils import check_banned_words
from python.generators.sql_processing.utils import check_banned_create_table_as
from python.generators.sql_processing.utils import DROP_TABLE_VIEW_PATTERN
from python.generators.sql_processing.utils import CREATE_TABLE_VIEW_PATTERN

CREATE_TABLE_ALLOWLIST = {
    ('/src/trace_processor/metrics/sql/android'
     '/android_blocking_calls_cuj_metric.sql'): [
        'android_cujs', 'relevant_binder_calls_with_names',
        'android_blocking_calls_cuj_calls'
    ],
    '/src/trace_processor/metrics/sql/android/jank/cujs.sql': [
        'android_jank_cuj'
    ],
    '/src/trace_processor/metrics/sql/chrome/gesture_flow_event.sql': [
        '{{prefix}}_latency_info_flow_step_filtered'
    ],
    '/src/trace_processor/metrics/sql/chrome/gesture_jank.sql': [
        '{{prefix}}_jank_maybe_null_prev_and_next_without_precompute'
    ],
    '/src/trace_processor/metrics/sql/experimental/frame_times.sql': [
        'DisplayCompositorPresentationEvents'
    ]
}


def match_create_table_pattern_to_dict(
    sql: str, pattern: str) -> Dict[str, Tuple[int, str]]:
  res = {}
  for line_num, matches in match_pattern(pattern, sql).items():
    res[matches[2]] = [line_num, str(matches[1])]
  return res


def match_drop_view_pattern_to_dict(sql: str,
                                    pattern: str) -> Dict[str, Tuple[int, str]]:
  res = {}
  for line_num, matches in match_pattern(pattern, sql).items():
    res[matches[1]] = [line_num, str(matches[0])]
  return res


def check(path: str) -> List[str]:
  with open(path) as f:
    sql = f.read()

  # Check that CREATE VIEW/TABLE has a matching DROP VIEW/TABLE before it.
  create_table_view_dir = match_create_table_pattern_to_dict(
      sql, CREATE_TABLE_VIEW_PATTERN)
  drop_table_view_dir = match_drop_view_pattern_to_dict(
      sql, DROP_TABLE_VIEW_PATTERN)
  errors = check_banned_create_table_as(sql,
                                        path.split(ROOT_DIR)[1],
                                        CREATE_TABLE_ALLOWLIST)
  for name, [line, type] in create_table_view_dir.items():
    if name not in drop_table_view_dir:
      errors.append(f'Missing DROP before CREATE {type.upper()} "{name}"\n'
                    f'Offending file: {path}\n')
      continue
    drop_line, drop_type = drop_table_view_dir[name]
    if drop_line > line:
      errors.append(f'DROP has to be before CREATE {type.upper()} "{name}"\n'
                    f'Offending file: {path}\n')
      continue
    if drop_type != type:
      errors.append(f'DROP type doesnt match CREATE {type.upper()} "{name}"\n'
                    f'Offending file: {path}\n')

  errors += check_banned_words(sql, path)
  return errors


def main():
  errors = []
  metrics_sources = os.path.join(ROOT_DIR, 'src', 'trace_processor', 'metrics',
                                 'sql')
  for root, _, files in os.walk(metrics_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if path.endswith('.sql'):
        errors += check(path)

  if errors:
    sys.stderr.write("\n".join(errors))
    sys.stderr.write("\n")
  return 0 if not errors else 1


if __name__ == '__main__':
  sys.exit(main())
