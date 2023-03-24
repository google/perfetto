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
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import parse_tables_from_files
#pylint: enable=wrong-import-position


@dataclass
class Header:
  """Represents a Python module which will be converted to a header."""
  out_path: str
  relout_path: str
  tables: List[ParsedTable]


def main():
  """Main function."""
  parser = argparse.ArgumentParser()
  parser.add_argument('--gen-dir', required=True)
  parser.add_argument('--inputs', required=True, nargs='*')
  parser.add_argument('--outputs', required=True, nargs='*')
  args = parser.parse_args()

  if len(args.inputs) != len(args.outputs):
    raise Exception('Number of inputs must match number of outputs')

  in_to_out = dict(zip(args.inputs, args.outputs))
  headers: Dict[str, Header] = {}
  for table in parse_tables_from_files(args.inputs):
    out_path = in_to_out[table.input_path]
    relout_path = os.path.relpath(out_path, args.gen_dir)

    header = headers.get(table.input_path, Header(out_path, relout_path, []))
    header.tables.append(table)
    headers[table.input_path] = header

  # Build a mapping from table class name to the output path of the header
  # which will be generated for it. This is used to include one header into
  # another for Id dependencies.
  table_class_name_to_relout = {}
  for header in headers.values():
    for table in header.tables:
      table_class_name_to_relout[table.table.class_name] = header.relout_path

  for header in headers.values():
    # Find all headers depended on by this table. These will be #include-ed when
    # generating the header file below so ensure we remove ourself.
    header_relout_deps: Set[str] = set()
    for table in header.tables:
      header_relout_deps.union(
          table_class_name_to_relout[c] for c in table.find_table_deps())
    header_relout_deps.discard(header.relout_path)

    with open(header.out_path, 'w', encoding='utf8') as out:
      ifdef_guard = re.sub(r'[^a-zA-Z0-9_-]', '_',
                           header.relout_path).upper() + '_'
      out.write(
          serialize_header(ifdef_guard, header.tables,
                           sorted(header_relout_deps)))
      out.write('\n')


if __name__ == '__main__':
  sys.exit(main())
