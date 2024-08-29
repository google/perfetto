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

# This tool checks that every SQL object created without prefix
# '_' is documented with proper schema.

import argparse
from typing import List, Tuple
import os
import sys
import re

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

from python.generators.sql_processing.docs_parse import ParsedModule
from python.generators.sql_processing.docs_parse import parse_file
from python.generators.sql_processing.utils import check_banned_create_table_as
from python.generators.sql_processing.utils import check_banned_create_view_as
from python.generators.sql_processing.utils import check_banned_words
from python.generators.sql_processing.utils import check_banned_drop
from python.generators.sql_processing.utils import check_banned_include_all


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '--stdlib-sources',
      default=os.path.join(ROOT_DIR, "src", "trace_processor", "perfetto_sql",
                           "stdlib"))
  parser.add_argument(
      '--verbose',
      action='store_true',
      default=False,
      help='Enable additional logging')
  parser.add_argument(
      '--name-filter',
      default=None,
      type=str,
      help='Filter the name of the modules to check (regex syntax)')

  args = parser.parse_args()
  modules: List[Tuple[str, str, ParsedModule]] = []
  for root, _, files in os.walk(args.stdlib_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if not path.endswith(".sql"):
        continue
      rel_path = os.path.relpath(path, args.stdlib_sources)
      if args.name_filter is not None:
        pattern = re.compile(args.name_filter)
        if not pattern.match(rel_path):
          continue

      with open(path, 'r') as f:
        sql = f.read()

      parsed = parse_file(rel_path, sql)

      # Some modules (i.e. `deprecated`) should not be checked.
      if not parsed:
        continue

      modules.append((path, sql, parsed))

      if args.verbose:
        obj_count = len(parsed.functions) + len(parsed.table_functions) + len(
            parsed.table_views) + len(parsed.macros)
        print(
            f"Parsing '{rel_path}' ({obj_count} objects, "
            f"{len(parsed.errors)} errors) - "
            f"{len(parsed.functions)} functions, "
            f"{len(parsed.table_functions)} table functions, "
            f"{len(parsed.table_views)} tables/views, "
            f"{len(parsed.macros)} macros.")

  all_errors = 0
  for path, sql, parsed in modules:
    errors = []
    lines = [l.strip() for l in sql.split('\n')]
    for line in lines:
      if line.startswith('--'):
        continue
      if 'run_metric' in line.casefold():
        errors.append("RUN_METRIC is banned in standard library.")
      if 'insert into' in line.casefold():
        errors.append("INSERT INTO table is not allowed in standard library.")

    # Validate includes
    package = parsed.package_name
    for include in parsed.includes:
      package = package.lower()
      include_package = include.package.lower()

      if (include_package == "common"):
        errors.append(
            "Common module has been deprecated in the standard library.")

      if (package != "viz" and include_package == "viz"):
        errors.append("No modules can depend on 'viz' outside 'viz' package.")

      if (package == "chrome" and include_package == "android"):
        errors.append(
            f"Modules from package 'chrome' can't include '{include.module}' "
            f"from package 'android'")

      if (package == "android" and include_package == "chrome"):
        errors.append(
            f"Modules from package 'android' can't include '{include.module}' "
            f"from package 'chrome'")

    errors += [
        *parsed.errors, *check_banned_words(sql),
        *check_banned_create_table_as(sql), *check_banned_create_view_as(sql),
        *check_banned_include_all(sql), *check_banned_drop(sql)
    ]

    if errors:
      sys.stderr.write(
          f"\nFound {len(errors)} errors in file '{path.split(ROOT_DIR)[1]}':\n- "
      )
      sys.stderr.write("\n- ".join(errors))
      sys.stderr.write("\n\n")

    all_errors += len(errors)

  return 0 if not all_errors else 1


if __name__ == "__main__":
  sys.exit(main())
