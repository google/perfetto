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
"""Generates the stdlib metadata JSON used by the UI.

The UI gets the stdlib docs themselves (tables, functions, columns etc.) from
trace processor. This script emits only the metadata trace processor does not
expose: tags, includes, data availability checks and table importance.
"""

import argparse
import json
import os
import sys
from typing import List, Tuple

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(ROOT_DIR)

from python.generators.sql_processing.docs_parse import ParsedModule
from python.generators.sql_processing.stdlib_parser import parse_all_modules
from python.generators.sql_processing.stdlib_tags import get_tags, get_table_importance
from python.perfetto.trace_data_checks import check_to_query, MODULE_DATA_CHECK_SQL, TABLE_DATA_CHECK_SQL

STDLIB_DIR = os.path.join(ROOT_DIR, 'src', 'trace_processor', 'perfetto_sql',
                          'stdlib')


def _format_metadata(modules: List[Tuple[str, str, str, ParsedModule]]) -> dict:
  """Format only the metadata not available from the TP table functions.

  The TP exposes module names, packages, table/function/macro names,
  descriptions, types, columns and args. This function emits only the
  complementary metadata that lives outside the SQL syntax:
    - tags and data-availability check SQL (module level)
    - includes (INCLUDE PERFETTO MODULE directives)
    - importance and data-availability check SQL (table level)

  Output (keyed by module name so the UI can look up by key):
  {
    "android.memory": {
      "tags": ["android"],
      "includes": ["android.memory.heap"],
      "data_check_sql": "SELECT EXISTS(...) AS has_data",  // null if absent
      "tables": {                                          // omitted if empty
        "android_heap_profile_allocation": {
          "importance": "high",                           // null if absent
          "data_check_sql": "SELECT EXISTS(...) AS has_data"  // null if absent
        }
      }
    },
    ...
  }
  """
  result = {}

  for _, _, module_name, parsed in modules:
    tags = get_tags(module_name)
    includes = [inc.module for inc in parsed.includes]
    data_check_sql = (
        check_to_query(MODULE_DATA_CHECK_SQL[module_name])
        if module_name in MODULE_DATA_CHECK_SQL else None)

    tables = {}
    for table in parsed.table_views:
      importance = get_table_importance(table.name)
      table_check = (
          check_to_query(TABLE_DATA_CHECK_SQL[table.name])
          if table.name in TABLE_DATA_CHECK_SQL else None)
      if importance is not None or table_check is not None:
        tables[table.name] = {
            'importance': importance,
            'data_check_sql': table_check,
        }

    entry = {
        'tags': tags,
        'includes': includes,
        'data_check_sql': data_check_sql,
    }
    if tables:
      entry['tables'] = tables

    result[module_name] = entry

  return result


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--out', required=True)
  args = parser.parse_args()

  metadata = _format_metadata(parse_all_modules(STDLIB_DIR))
  with open(args.out, 'w', encoding='utf-8') as f:
    json.dump(metadata, f)
  return 0


if __name__ == '__main__':
  sys.exit(main())
