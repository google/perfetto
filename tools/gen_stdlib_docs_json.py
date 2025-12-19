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
"""
Generate stdlib documentation JSON.

This tool generates documentation JSON for the Perfetto SQL standard library.
"""

import argparse
import json
import os
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(ROOT_DIR)

from python.generators.sql_processing.stdlib_parser import parse_all_modules, format_docs


def main():
  parser = argparse.ArgumentParser(
      description="Generate stdlib documentation JSON")
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

  # Get list of SQL files from either input-list-file or arguments
  if args.input_list_file:
    with open(args.input_list_file, 'r', encoding='utf-8') as f:
      sql_files = [line.strip() for line in f.readlines() if line.strip()]
  else:
    sql_files = args.sql_files

  if not sql_files:
    print("No SQL files provided", file=sys.stderr)
    return 1

  # Find stdlib path from the common path of all files
  # Unfortunately we cannot pass this in as an arg as soong does not provide
  # us a way to get the path to the Perfetto source directory.
  stdlib_path = os.path.commonpath(sql_files)

  # Validate paths (check for ../)
  for file_name in sql_files:
    relpath = os.path.relpath(file_name, stdlib_path)
    # We've had bugs (e.g. b/264711057) when Soong's common path logic breaks
    # and ends up with a bunch of ../ prefixing the path: disallow any ../
    # as this should never be a valid in our C++ output.
    if '../' in relpath:
      raise ValueError(
          f"Invalid path with parent directory reference: {relpath}")

  # Parse all modules using the library
  try:
    modules = parse_all_modules(
        stdlib_path=stdlib_path,
        include_internal=args.with_internal,
        name_filter=None)

    # Format as docs JSON
    output_data = format_docs(modules)

    # Write output
    with open(args.json_out, 'w', encoding='utf-8') as f:
      json.dump(output_data, f, indent=None if args.minify else 2)

    return 0
  except Exception as e:
    print(f"Error generating docs JSON: {e}", file=sys.stderr)
    import traceback
    traceback.print_exc()
    return 1


if __name__ == '__main__':
  sys.exit(main())
