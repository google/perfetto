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
import sys
from typing import Any
from typing import Dict
from typing import Optional

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import ColumnFlag
import python.generators.trace_processor_table.util as util
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import ParsedColumn
#pylint: enable=wrong-import-position


def gen_json_for_column(table: ParsedTable,
                        col: ParsedColumn) -> Optional[Dict[str, Any]]:
  """Generates the JSON documentation for a column in a table."""
  assert table.table.tabledoc

  # id and type columns should be skipped if the table specifies so.
  is_skippable_col = col.is_implicit_id or col.is_implicit_type
  if table.table.tabledoc.skip_id_and_type and is_skippable_col:
    return None

  # Ignore hidden columns in the documentation.
  if ColumnFlag.HIDDEN in col.column.flags:
    return None

  # Our default assumption is the documentation for a column is a plain string
  # so just make the comment for the column equal to that.

  if isinstance(col.doc, ColumnDoc):
    comment = col.doc.doc
    if col.doc.joinable:
      join_table, join_col = col.doc.joinable.split('.')
    else:
      join_table, join_col = None, None
  elif isinstance(col.doc, str):
    comment = col.doc
    join_table, join_col = None, None
  else:
    raise Exception('Unknown column documentation type '
                    f'{table.table.class_name}::{col.column.name}')

  parsed_type = util.parse_type_with_cols(table.table,
                                          [c.column for c in table.columns],
                                          col.column.type)
  docs_type = parsed_type.cpp_type
  if docs_type == 'StringPool::Id':
    docs_type = 'string'

  ref_class_name = None
  if parsed_type.id_table and not col.is_implicit_id:
    id_table_name = util.public_sql_name(parsed_type.id_table)
    ref_class_name = parsed_type.id_table.class_name

    if not join_table and not join_col:
      join_table = id_table_name
      join_col = "id"

  return {
      'name': col.column.name,
      'type': docs_type,
      'comment': comment,
      'optional': parsed_type.is_optional,
      'refTableCppName': ref_class_name,
      'joinTable': join_table,
      'joinCol': join_col,
  }


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--out', required=True)
  parser.add_argument('inputs', nargs='*')
  parser.add_argument('--relative-input-dir')
  args = parser.parse_args()

  def get_relin_path(in_path: str):
    if not args.relative_input_dir:
      return in_path
    return os.path.relpath(in_path, args.relative_input_dir)

  modules = [
      os.path.splitext(get_relin_path(i).replace('/', '.'))[0]
      for i in args.inputs
  ]
  table_docs = []
  for parsed in util.parse_tables_from_modules(modules):
    table = parsed.table

    # If there is no non-intrinsic alias for the table, don't
    # include the table in the docs.
    name = util.public_sql_name(table)
    if name.startswith('__intrinsic_') or name.startswith('experimental_'):
      continue

    doc = table.tabledoc
    assert doc
    cols = (
        gen_json_for_column(parsed, c)
        for c in parsed.columns
        if not c.is_ancestor)
    table_docs.append({
        'name': name,
        'cppClassName': table.class_name,
        'defMacro': table.class_name,
        'comment': '\n'.join(l.strip() for l in doc.doc.splitlines()),
        'parent': None,
        'parentDefName': table.parent.class_name if table.parent else '',
        'tablegroup': doc.group,
        'cols': [c for c in cols if c]
    })

  with open(args.out, 'w') as out:
    json.dump(table_docs, out, indent=2)
    out.write('\n')


if __name__ == '__main__':
  exit(main())
