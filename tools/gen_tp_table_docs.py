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

import argparse
import json
import os
import runpy
import sys
from typing import Any
from typing import Dict
from typing import List
from typing import Optional
from typing import Union

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.trace_processor_table.public import Column
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import Table
import python.generators.trace_processor_table.util as util


def gen_json_for_column(table: Table, col: Column,
                        doc: Union[ColumnDoc, str]) -> Optional[Dict[str, Any]]:
  """Generates the JSON documentation for a column in a table."""

  # id and type columns should be skipped if the table specifies so.
  is_skippable_col = col._is_auto_added_id or col._is_auto_added_type
  if table.tabledoc.skip_id_and_type and is_skippable_col:
    return None

  # Our default assumption is the documentation for a column is a plain string
  # so just make the comment for the column equal to that.

  if isinstance(doc, ColumnDoc):
    comment = doc.doc
    if doc.joinable:
      join_table, join_type = doc.joinable.split('.')
    else:
      join_table, join_type = None, None
  elif isinstance(doc, str):
    comment = doc
    join_table, join_type = None, None
  else:
    raise Exception('Unknown column documentation type')

  parsed_type = util.parse_type(table, col.type)
  docs_type = parsed_type.cpp_type
  if docs_type == 'StringPool::Id':
    docs_type = 'string'

  ref_class_name = None
  if parsed_type.id_table and not col._is_auto_added_id:
    id_table_name = util.public_sql_name_for_table(parsed_type.id_table)
    ref_class_name = parsed_type.id_table.class_name

    # We shouldn't really specify the join tables when it's a simple id join.
    assert join_table is None
    assert join_type is None

    join_table = id_table_name
    join_type = "id"

  return {
      'name': col.name,
      'type': docs_type,
      'comment': comment,
      'optional': parsed_type.is_optional,
      'refTableCppName': ref_class_name,
      'joinTable': join_table,
      'joinCol': join_type,
  }


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--out', required=True)
  parser.add_argument('inputs', nargs='*')
  args = parser.parse_args()

  tables: List[Table] = []
  for in_path in args.inputs:
    for table in runpy.run_path(in_path)['ALL_TABLES']:
      tables.append(util.augment_table_with_auto_cols(table))

  table_docs = []
  for table in tables:
    doc = table.tabledoc
    cols = (
        gen_json_for_column(table, c, doc.columns[c.name])
        for c in table.columns)
    table_docs.append({
        'name': util.public_sql_name_for_table(table),
        'cppClassName': table.class_name,
        'defMacro': table.class_name,
        'comment': doc.doc,
        'parent': None,
        'parentDefName': '',
        'tablegroup': doc.group,
        'cols': [c for c in cols if c]
    })

  with open(args.out, 'w') as out:
    json.dump(table_docs, out, indent=2)
    out.write('\n')


if __name__ == '__main__':
  exit(main())
