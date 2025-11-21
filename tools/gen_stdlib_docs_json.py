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
import os
import sys
import json
import re
from collections import defaultdict
from typing import Dict, Tuple, Optional

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.docs_parse import DocParseOptions, parse_file


def _is_internal(name: str) -> bool:
  """Check if a name represents an internal artifact (starts with _)."""
  return name.startswith('_')


def _summary_desc(s: str) -> str:
  """Extract the first sentence from a description."""
  return s.split('. ')[0].replace('\n', ' ')


def _long_type_to_table(s: str) -> Tuple[Optional[str], Optional[str]]:
  """Parse a long type string to extract table and column references.

  Expected format: "TYPE(table_name.column_name)"
  Returns (table_name, column_name) or (None, None) if no match.
  """
  # Match pattern like "JOIN_TYPE(table.column)" where the period is literal
  pattern = r'[A-Z]*\(([a-z_]*)\.([a-z_]*)\)'
  m = re.match(pattern, s)
  if not m:
    return (None, None)
  g = m.groups()
  return (g[0], g[1])


def _create_field_dict(name: str, obj, include_desc: bool = True) -> dict:
  """Create a dictionary for a column or argument with table/column references."""
  table, column = _long_type_to_table(obj.long_type)
  result = {
      'name': name,
      'type': obj.long_type,
      'table': table,
      'column': column,
  }
  if include_desc:
    result['desc'] = obj.description
  return result


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--json-out', required=True)
  parser.add_argument('--input-list-file')
  parser.add_argument(
      '--minify',
      action='store_true',
      help='Minify JSON output (removes indentation and whitespace)')
  parser.add_argument(
      '--with-internal',
      action='store_true',
      help='Include internal artifacts (those starting with _) in output')
  parser.add_argument('sql_files', nargs='*')
  args = parser.parse_args()

  if args.input_list_file and args.sql_files:
    print(
        "Only one of --input-list-file and list of SQL files expected",
        file=sys.stderr)
    return 1

  sql_files = []
  if args.input_list_file:
    with open(args.input_list_file, 'r', encoding='utf-8') as input_list_file:
      for line in input_list_file.read().splitlines():
        sql_files.append(line)
  else:
    sql_files = args.sql_files

  # Unfortunately we cannot pass this in as an arg as soong does not provide
  # us a way to get the path to the Perfetto source directory. This fails on
  # empty path but it's a price worth paying to have to use gross hacks in
  # Soong.
  root_dir = os.path.commonpath(sql_files)

  # Extract the SQL output from each file.
  sql_outputs: Dict[str, str] = {}
  for file_name in sql_files:
    with open(file_name, 'r', encoding='utf-8') as f:
      relpath = os.path.relpath(file_name, root_dir)

      # We've had bugs (e.g. b/264711057) when Soong's common path logic breaks
      # and ends up with a bunch of ../ prefixing the path: disallow any ../
      # as this should never be a valid in our C++ output.
      if '../' in relpath:
        raise ValueError(
            f"Invalid path with parent directory reference: {relpath}")

      sql_outputs[relpath] = f.read()

  packages = defaultdict(list)
  for path, sql in sql_outputs.items():
    package_name = path.split("/")[0]
    module_name = path.split(".sql")[0].replace("/", ".")

    docs = parse_file(
        path,
        sql,
        options=DocParseOptions(
            enforce_every_column_set_is_documented=True,
            include_internal=args.with_internal),
    )

    # Some modules (i.e `deprecated`) should not generate docs.
    if not docs:
      continue

    if len(docs.errors) > 0:
      for e in docs.errors:
        print(e, file=sys.stderr)
      return 1

    module_dict = {
        'module_name':
            module_name,
        'module_doc': {
            'name': docs.module_doc.name,
            'desc': docs.module_doc.desc,
        } if docs.module_doc else None,
        'data_objects': [{
            'name':
                table.name,
            'desc':
                table.desc,
            'summary_desc':
                _summary_desc(table.desc),
            'type':
                table.type,
            'visibility':
                'private' if _is_internal(table.name) else 'public',
            'cols': [
                _create_field_dict(col_name, col)
                for (col_name, col) in table.cols.items()
            ]
        }
                         for table in docs.table_views],
        'functions': [{
            'name':
                function.name,
            'desc':
                function.desc,
            'summary_desc':
                _summary_desc(function.desc),
            'visibility':
                'private' if _is_internal(function.name) else 'public',
            'args': [
                _create_field_dict(arg_name, arg)
                for (arg_name, arg) in function.args.items()
            ],
            'return_type':
                function.return_type,
            'return_desc':
                function.return_desc,
        }
                      for function in docs.functions],
        'table_functions': [{
            'name':
                function.name,
            'desc':
                function.desc,
            'summary_desc':
                _summary_desc(function.desc),
            'visibility':
                'private' if _is_internal(function.name) else 'public',
            'args': [
                _create_field_dict(arg_name, arg)
                for (arg_name, arg) in function.args.items()
            ],
            'cols': [
                _create_field_dict(col_name, col)
                for (col_name, col) in function.cols.items()
            ]
        }
                            for function in docs.table_functions],
        'macros': [{
            'name':
                macro.name,
            'desc':
                macro.desc,
            'summary_desc':
                _summary_desc(macro.desc),
            'visibility':
                'private' if _is_internal(macro.name) else 'public',
            'return_desc':
                macro.return_desc,
            'return_type':
                macro.return_type,
            'args': [
                _create_field_dict(arg_name, arg)
                for (arg_name, arg) in macro.args.items()
            ],
        }
                   for macro in docs.macros],
    }
    packages[package_name].append(module_dict)

  packages_list = [{
      "name": name,
      "modules": modules
  } for name, modules in packages.items()]

  with open(args.json_out, 'w+', encoding='utf-8') as f:
    json.dump(packages_list, f, indent=None if args.minify else 4)

  return 0


if __name__ == '__main__':
  sys.exit(main())
