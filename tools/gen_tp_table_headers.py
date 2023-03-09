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
import runpy
import sys
from typing import List
from typing import Set

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.generators.trace_processor_table.public import Alias
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.serialize import serialize_header
from python.generators.trace_processor_table.util import find_table_deps
from python.generators.trace_processor_table.util import augment_table_with_auto_cols
from python.generators.trace_processor_table.util import topological_sort_tables
#pylint: enable=wrong-import-position


@dataclass
class Header:
  """Represents a Python module which will be converted to a header."""
  out_path: str
  relout_path: str
  tables: List[Table]


def normalize_table_for_serialization(table: Table) -> Table:
  """Normalize the table for generating headers.

  Normalizing = taking the table the user define and converting it into
  the form needed by the seralizer. Speficially this means:
  1. Adding the 'id' and 'type" columns.
  2. Removing any alias columns (for now, these are handled in SQL not C++.
     This may change in the future.
  """
  augmented = augment_table_with_auto_cols(table)
  augmented.columns = [
      c for c in augmented.columns if not isinstance(c.type, Alias)
  ]
  return augmented


def main():
  """Main function."""
  parser = argparse.ArgumentParser()
  parser.add_argument('--gen-dir', required=True)
  parser.add_argument('--inputs', required=True, nargs='*')
  parser.add_argument('--outputs', required=True, nargs='*')
  args = parser.parse_args()

  if len(args.inputs) != len(args.outputs):
    raise Exception('Number of inputs must match number of outputs')

  headers: List[Header] = []
  for (in_path, out_path) in zip(args.inputs, args.outputs):
    tables = runpy.run_path(in_path)['ALL_TABLES']
    relout_path = os.path.relpath(out_path, args.gen_dir)
    headers.append(Header(out_path, relout_path, tables))

  # Build a mapping from table class name to the output path of the header
  # which will be generated for it. This is used to include one header into
  # another for Id dependencies.
  table_class_name_to_relout = {}
  for header in headers:
    for table in header.tables:
      table_class_name_to_relout[table.class_name] = header.relout_path

  for header in headers:
    # Topologically sort the tables in this header to ensure that any deps are
    # defined *before* the table itself.
    sorted_tables = topological_sort_tables(
        [normalize_table_for_serialization(table) for table in header.tables])

    # Find all headers depended on by this table. These will be #include-ed when
    # generating the header file below so ensure we remove ourself.
    header_relout_deps: Set[str] = set()
    for table in sorted_tables:
      header_relout_deps.union(
          table_class_name_to_relout[c] for c in find_table_deps(table))
    header_relout_deps.discard(header.relout_path)

    with open(header.out_path, 'w', encoding='utf8') as out:
      ifdef_guard = re.sub(r'[^a-zA-Z0-9_-]', '_',
                           header.relout_path).upper() + '_'
      out.write(
          serialize_header(ifdef_guard, sorted_tables,
                           sorted(header_relout_deps)))
      out.write('\n')


if __name__ == '__main__':
  sys.exit(main())
