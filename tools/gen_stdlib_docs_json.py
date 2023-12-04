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
from collections import defaultdict
from typing import Dict

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.docs_parse import parse_file


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--json-out', required=True)
  parser.add_argument('--input-list-file')
  parser.add_argument('sql_files', nargs='*')
  args = parser.parse_args()

  if args.input_list_file and args.sql_files:
    print("Only one of --input-list-file and list of SQL files expected")
    return 1

  sql_files = []
  if args.input_list_file:
    with open(args.input_list_file, 'r') as input_list_file:
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
    with open(file_name, 'r') as f:
      relpath = os.path.relpath(file_name, root_dir)

      # We've had bugs (e.g. b/264711057) when Soong's common path logic breaks
      # and ends up with a bunch of ../ prefixing the path: disallow any ../
      # as this should never be a valid in our C++ output.
      assert '../' not in relpath

      sql_outputs[relpath] = f.read()

  modules = defaultdict(list)
  # Add documentation from each file
  for path, sql in sql_outputs.items():
    module_name = path.split("/")[0]
    import_key = path.split(".sql")[0].replace("/", ".")

    docs = parse_file(path, sql)
    if len(docs.errors) > 0:
      for e in docs.errors:
        print(e)
      return 1

    file_dict = {
        'import_key':
            import_key,
        'imports': [{
            'name': table.name,
            'desc': table.desc,
            'type': table.type,
            'cols': {
                col_name: {
                    'type': col.type,
                    'desc': col.description,
                } for (col_name, col) in table.cols.items()
            },
        } for table in docs.table_views],
        'functions': [{
            'name': function.name,
            'desc': function.desc,
            'args': {
                arg_name: {
                    'type': arg.type,
                    'desc': arg.description,
                } for (arg_name, arg) in function.args.items()
            },
            'return_type': function.return_type,
            'return_desc': function.return_desc,
        } for function in docs.functions],
        'table_functions': [{
            'name': function.name,
            'desc': function.desc,
            'args': {
                arg_name: {
                    'type': arg.type,
                    'desc': arg.description,
                } for (arg_name, arg) in function.args.items()
            },
            'cols': {
                col_name: {
                    'type': col.type,
                    'desc': col.description,
                } for (col_name, col) in function.cols.items()
            },
        } for function in docs.table_functions],
    }
    modules[module_name].append(file_dict)

  with open(args.json_out, 'w+') as f:
    json.dump(modules, f, indent=4)

  return 0


if __name__ == '__main__':
  sys.exit(main())
