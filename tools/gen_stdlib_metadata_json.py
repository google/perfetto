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

The UI gets the stdlib docs themselves (tables, functions, columns etc.) and
module includes from trace processor. This script emits only the metadata trace
processor does not expose: tags, data availability checks and table importance.
"""

import argparse
import json
import os
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(ROOT_DIR)

from python.generators.sql_processing.stdlib_tags import MODULE_TAGS, TABLE_IMPORTANCE, get_tags, get_table_importance
from python.perfetto.trace_data_checks import check_to_query, MODULE_DATA_CHECK_SQL, TABLE_DATA_CHECK_SQL


def _format_metadata() -> dict:
  """Format the metadata not available from the TP table functions.

  Output (keyed by name so the UI can look up by key):
  {
    "modules": {
      "android.memory": {
        "tags": ["android"],
        "data_check_sql": "SELECT EXISTS(...) AS has_data"  // null if absent
      }
    },
    "tables": {
      "android_heap_profile_allocation": {
        "importance": "high",                               // null if absent
        "data_check_sql": "SELECT EXISTS(...) AS has_data"  // null if absent
      }
    }
  }
  """
  modules = {}
  for name in set(MODULE_TAGS) | set(MODULE_DATA_CHECK_SQL):
    modules[name] = {
        'tags':
            get_tags(name),
        'data_check_sql': (check_to_query(MODULE_DATA_CHECK_SQL[name])
                           if name in MODULE_DATA_CHECK_SQL else None),
    }

  tables = {}
  for name in set(TABLE_IMPORTANCE) | set(TABLE_DATA_CHECK_SQL):
    tables[name] = {
        'importance':
            get_table_importance(name),
        'data_check_sql': (check_to_query(TABLE_DATA_CHECK_SQL[name])
                           if name in TABLE_DATA_CHECK_SQL else None),
    }

  return {'modules': modules, 'tables': tables}


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--out', required=True)
  args = parser.parse_args()

  metadata = _format_metadata()
  with open(args.out, 'w', encoding='utf-8') as f:
    json.dump(metadata, f)
  return 0


if __name__ == '__main__':
  sys.exit(main())
