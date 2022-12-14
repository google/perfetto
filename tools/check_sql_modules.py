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

# This tool checks that every create (table|view) without prefix
# 'internal_' is documented with proper schema.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import re
import sys
from sql_modules_utils import *

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Check that CREATE VIEW/TABLE has a matching schema before it.
def check_create_table_view(path, module, sql):
  errors = 0
  obj_name, schema_cols, schema_desc = None, False, False
  lines = sql.split('\n')
  for i, line in enumerate(lines):
    create_line = re.match(create_table_view_pattern(), line)

    # Ignore all lines that don't create an object
    if create_line is None:
      continue

    obj_name = create_line.group(2)

    # Ignore 'internal_' tables|views
    if re.match(r'^internal_.*', obj_name):
      continue

    # Check whether the name starts with module_name
    starts_with_module_name = re.match(f'^{module}_.*', obj_name)
    if module == 'common':
      if starts_with_module_name:
        sys.stderr.write(
            f"Invalid name in module {obj_name}. "
            f"In module 'common' the name shouldn't start with 'common_'.\n")
        errors += 1
    else:
      if not starts_with_module_name:
        sys.stderr.write(f"Invalid name in module {obj_name}. "
                         f"View/table name has to begin with {module}_.\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1

    # Validate the schema before the create line.
    for comment_line in fetch_comment(lines[i - 1::-1]):
      # Ignore only '--' line.
      if comment_line == '--':
        continue

      # Break on SQL lines (lines with words without '--' at the beginning)
      # and empty lines.
      if not line or not comment_line.startswith('--'):
        break

      # Look for '-- @column' line as a column description
      column_line = re.match(column_pattern(), comment_line)
      if column_line is not None:
        if not schema_desc:
          sys.stderr.write(f"Columns needs to be defined after description.\n")
          sys.stderr.write(f'{path}:\n"{comment_line}"\n')
          errors += 1
          continue

        schema_cols = True
        continue

      # The only  option left is a description, but it has to be after
      # schema columns.
      schema_desc = True

    if not schema_cols:
      sys.stderr.write((f"Missing documentation schema for {obj_name}\n"))
      sys.stderr.write(f'{path}:\n"{line}"\n')
      errors += 1
    obj_name, schema_cols, schema_desc = None, False, False

  return errors


def parse_args(args_str):
  errors = 0
  args = {}
  for arg_str in args_str.split(","):
    m = re.match(arg_pattern(), arg_str)
    if m is None:
      sys.stderr.write(f"Wrong arguments formatting for '{arg_str}'\n")
      errors += 1
      continue
    args[m.group(1)] = m.group(2)
  return errors, args


# Check that CREATE_FUNCTION has a matching schema before it.
def match_create_functions(sql):
  errors = 0

  line_to_match_dict = match_pattern(create_function_pattern(), sql)
  if line_to_match_dict:
    return []

  functions = {}
  for line_id, match_groups in line_to_match_dict.items():
    name = match_groups[0]
    if re.match(r'^INTERNAL_.*', name):
      continue

    parse_errors, args = parse_args(match_groups[1])
    errors += parse_errors
    functions[line_id] = dict(
        name=name, args=args, ret_type=match_groups[2], sql=match_groups[3])

  return dict(sorted(functions.items()))


def check_function_docs(path, rev_comment, fun_data):
  errors = 0
  has_ret, has_args, has_desc = False, False, False

  for line in rev_comment:
    # Break if the comment is finished
    if not line or not line.startswith('--'):
      break

    # Ignore empty lines
    if line == "--":
      continue

    if line.startswith('-- @ret'):
      if has_ret:
        sys.stderr.write(f"Function can only return one element: '{line}'\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      m = re.match(function_return_pattern(), line)
      if m is None:
        sys.stderr.write("The return docs formatting is wrong. It should be:\n"
                         "-- @ret [A-Z]* {desc}\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      if fun_data['ret_type'] != m.group(1):
        sys.stderr.write(
            f"The code specifies {fun_data['ret_type']} as return type, "
            f"but its {m.group(1)} in docs.\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      has_ret = True
      continue

    if line.startswith('-- @arg'):
      if not has_ret:
        sys.stderr.write(
            f"Arguments should be specified before return: '{line}'\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      m = re.match(args_pattern(), line)
      if m is None:
        sys.stderr.write("The arg docs formatting is wrong. It should be:\n"
                         "-- @arg [a-z_]* [A-Z]* {desc}\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      arg_name, arg_type = m.group(1), m.group(2)
      if arg_name not in fun_data['args']:
        sys.stderr.write(
            f"There is not argument '{arg_name} specified in code.\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      if arg_type != fun_data['args'][arg_name]:
        sys.stderr.write(
            f"In the code, the type of '{arg_name} is "
            f"{fun_data['args'][arg_name]}, but according to the docs "
            f"it is '{arg_type}.\n")
        sys.stderr.write(f'{path}:\n"{line}"\n')
        errors += 1
        continue

      has_args = True
      continue

    if has_args:
      has_desc = True
      return errors

  if not has_ret:
    sys.stderr.write(f"Return value was not specified in the documentation "
                     f"of function '{fun_data['name']}'.\n")
    sys.stderr.write(f'{path}')
    errors += 1
    return errors

  if not has_args:
    sys.stderr.write(f"Arguments were not specified in the documentation "
                     f"of function '{fun_data['name']}'.\n")
    sys.stderr.write(f'{path}')
    errors += 1
    return errors

  if not has_desc:
    sys.stderr.write(f"Missing description of function '{fun_data['name']}'.\n")
    sys.stderr.write(f'{path}')
    errors += 1
    return errors


def check_create_functions(path, module, sql):
  errors = 0
  matched_create_functions = match_create_functions(sql)

  if not bool(matched_create_functions):
    return errors

  lines = sql.split('\n')
  for line_id, fun_data in matched_create_functions.items():
    starts_with_module = fun_data['name'].startswith('{module}_'.upper())
    if module == 'common' and starts_with_module:
      sys.stderr.write(
          f"For module 'common', function name shouldn't start with "
          f"'COMMON_', as in {fun_data['name']}'.\n")
      sys.stderr.write(f'{path}')
      errors += 1
    if module != 'common' and not starts_with_module:
      sys.stderr.write(f"Function name ({fun_data['name']}) "
                       f"should start with '{module.upper()}_'\n")
      sys.stderr.write(f'{path}')
      errors += 1
    errors += check_function_docs(path, lines[line_id - 1::-1], fun_data)

  return errors


def check(path):
  errors = 0

  # Get module name
  module_name = path.split('/stdlib/')[-1].split('/')[0]

  with open(path) as f:
    sql = f.read()

  errors += check_create_table_view(path, module_name, sql)
  errors += check_create_functions(path, module_name, sql)
  return errors


def main():
  errors = 0
  metrics_sources = os.path.join(ROOT_DIR, 'src', 'trace_processor', 'stdlib')
  for root, _, files in os.walk(metrics_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if path.endswith('.sql'):
        errors += check(path)
  return 0 if errors == 0 else 1


if __name__ == '__main__':
  sys.exit(main())
