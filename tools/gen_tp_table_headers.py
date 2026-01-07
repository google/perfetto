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
sys.path.insert(0, os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.generators.trace_processor_table.serialize import serialize_header
from python.generators.trace_processor_table.serialize import serialize_fwd_header
from python.generators.trace_processor_table.util import find_table_deps
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import parse_tables_from_modules
#pylint: enable=wrong-import-position

# Suffix which replaces the .py extension for all input modules.
OUT_HEADER_SUFFIX = '_py.h'
OUT_FWD_HEADER_SUFFIX = '_fwd.h'


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

  def to_module_name(module_input: str):
    module = get_relin_path(module_input)
    if module.endswith('.py'):
      module = module[:-3]
    # On Windows the path can contain '/' or os.sep, depending on how this
    # script is executed. So we need to replace both.
    return module.replace('/', '.').replace(os.sep, '.')

  modules = [to_module_name(i) for i in args.inputs]
  headers: Dict[str, Header] = {}
  for table in parse_tables_from_modules(modules):
    input_path = get_relin_path_from_module_path(table.table.python_module)
    header = headers.get(input_path, Header([]))
    header.tables.append(table)
    headers[input_path] = header

  # Collect all tables and fwd header paths for generating all_tables_fwd.h
  all_tables: List[ParsedTable] = []
  all_fwd_header_paths: List[str] = []

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

    ifdef_guard = re.sub(r'[^a-zA-Z0-9_-]', '_', relout_path).upper() + '_'

    # Compute forward header path
    fwd_out_path = out_path.replace(OUT_HEADER_SUFFIX, OUT_FWD_HEADER_SUFFIX)
    fwd_relout_path = relout_path.replace(OUT_HEADER_SUFFIX,
                                          OUT_FWD_HEADER_SUFFIX)
    fwd_header_path = get_header_path(in_path).replace(OUT_HEADER_SUFFIX,
                                                       OUT_FWD_HEADER_SUFFIX)
    fwd_ifdef_guard = re.sub(r'[^a-zA-Z0-9_-]', '_',
                             fwd_relout_path).upper() + '_'

    # Collect for all_tables header
    all_tables.extend(header.tables)
    all_fwd_header_paths.append(fwd_header_path)

    # For fwd header, only need includes of other fwd headers
    fwd_deps = [
        p.replace(OUT_HEADER_SUFFIX, OUT_FWD_HEADER_SUFFIX)
        for p in sorted(header_relout_deps)
    ]

    # Generate the forward declaration header first
    with open(fwd_out_path, 'w', encoding='utf8') as out:
      out.write(serialize_fwd_header(fwd_ifdef_guard, header.tables, fwd_deps))
      out.write('\n')

    # Generate the full header (includes the fwd header)
    with open(out_path, 'w', encoding='utf8') as out:
      out.write(
          serialize_header(ifdef_guard, header.tables,
                           sorted(header_relout_deps), fwd_header_path))
      out.write('\n')

  # Generate the combined all_tables_fwd.h header
  if all_fwd_header_paths:
    first_fwd_path = all_fwd_header_paths[0]
    out_dir = os.path.dirname(first_fwd_path)
    generate_all_tables_header(args, all_tables, all_fwd_header_paths, out_dir)


def generate_all_tables_header(args, all_tables: List[ParsedTable],
                               fwd_header_paths: List[str], out_dir: str):
  """Generates an all_tables_fwd.h header with variant and count only."""
  rel_path = os.path.join(out_dir, 'all_tables_fwd.h')
  out_path = os.path.join(args.gen_dir, rel_path)
  ifdef_guard = re.sub(r'[^a-zA-Z0-9_-]', '_', rel_path).upper() + '_'

  includes = '\n'.join([
      f'#include "{p}"  // IWYU pragma: export'
      for p in sorted(fwd_header_paths)
  ])
  variant_entries = ', '.join([t.table.class_name for t in all_tables])

  content = f'''\
#ifndef {ifdef_guard}
#define {ifdef_guard}

#include <cstddef>
#include <variant>

#include "perfetto/ext/base/variant.h"

{includes}

namespace perfetto::trace_processor::tables {{

// Variant of all table types (use base::variant_index<AllTables, T>() to get index)
using AllTables = std::variant<{variant_entries}>;

// Count of all tables
inline constexpr size_t kTableCount = std::variant_size_v<AllTables>;

}}  // namespace perfetto::trace_processor::tables

#endif  // {ifdef_guard}
'''

  with open(out_path, 'w', encoding='utf8') as out:
    out.write(content)


if __name__ == '__main__':
  sys.exit(main())
