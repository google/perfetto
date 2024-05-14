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
from dataclasses import dataclass
import os
import re
import sys
from typing import Dict
from typing import List
from typing import Set

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.generators.trace_processor_table.serialize import serialize_header
from python.generators.trace_processor_table.util import find_table_deps
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import parse_tables_from_modules
#pylint: enable=wrong-import-position

# Suffix which replaces the .py extension for all input modules.
OUT_HEADER_SUFFIX = '_py.h'


@dataclass
class Header:
  """Represents a Python module which will be converted to a header."""
  tables: List[ParsedTable]


def main():
  """Main function."""
  parser = argparse.ArgumentParser()
  parser.add_argument('--inputs', required=True, nargs='*')
  parser.add_argument('--gen-dir', required=True)
  parser.add_argument('--relative-input-dir')
  parser.add_argument('--import-prefix', default='')
  args = parser.parse_args()

  def get_relin_path(in_path: str):
    if not args.relative_input_dir:
      return in_path
    return os.path.relpath(in_path, args.relative_input_dir)

  def get_relout_path(in_path: str):
    return os.path.splitext(in_path)[0] + OUT_HEADER_SUFFIX

  def get_out_path(in_path: str):
    return os.path.join(args.gen_dir, get_relout_path(in_path))

  def get_header_path(in_path: str):
    return os.path.join(args.import_prefix, get_relout_path(in_path))

  def get_relin_path_from_module_path(module_path: str):
    return module_path[module_path.rfind(os.sep + 'src') + 1:]

  modules = [
      # On Windows the path can contain '/' or os.sep, depending on how this
      # script is executed. So we need to replace both.
      os.path.splitext(
          get_relin_path(i).replace('/', '.').replace(os.sep, '.'))[0]
      for i in args.inputs
  ]
  headers: Dict[str, Header] = {}
  for table in parse_tables_from_modules(modules):
    input_path = get_relin_path_from_module_path(table.table.python_module)
    header = headers.get(input_path, Header([]))
    header.tables.append(table)
    headers[input_path] = header

  for in_path, header in headers.items():
    out_path = get_out_path(in_path)
    relout_path = get_relout_path(in_path)

    # Find all headers depended on by this table. These will be #include-ed when
    # generating the header file below so ensure we remove ourself.
    header_relout_deps: Set[str] = set()
    for table in header.tables:
      header_relout_deps = header_relout_deps.union([
          get_header_path(get_relin_path_from_module_path(c.python_module))
          for c in find_table_deps(table.table)
      ])
    header_relout_deps.discard(relout_path)

    with open(out_path, 'w', encoding='utf8') as out:
      ifdef_guard = re.sub(r'[^a-zA-Z0-9_-]', '_', relout_path).upper() + '_'
      out.write(
          serialize_header(ifdef_guard, header.tables,
                           sorted(header_relout_deps)))
      out.write('\n')


if __name__ == '__main__':
  sys.exit(main())
