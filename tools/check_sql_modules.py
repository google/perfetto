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
# 'internal_' is documented with proper schema.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import re
import sys
from sql_modules_utils import *
from typing import Union, List

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Stores documentation for CREATE {TABLE|VIEW} with comment split into
# segments.
class TableViewDocs:

  def __init__(self, name: str, desc: List[str], columns: List[str], path: str):
    self.name = name
    self.desc = desc
    self.columns = columns
    self.path = path

  # Contructs new TableViewDocs from whole comment, by splitting it on typed
  # lines. Returns None for improperly structured schemas.
  @staticmethod
  def create_from_comment(path: str, comment: List[str], module: str,
                          matches: tuple) -> tuple["TableViewDocs", List[str]]:
    obj_type, name = matches[:2]

    # Ignore internal tables and views.
    if re.match(r"^internal_.*", name):
      return None, []

    errors = validate_name(name, module)
    col_start = None

    # Splits code into segments by finding beginning of column segment.
    for i, line in enumerate(comment):
      # Ignore only '--' line.
      if line == "--":
        continue

      m = re.match(typed_comment_pattern(), line)

      # Ignore untyped lines
      if not m:
        continue

      line_type = m.group(1)
      if line_type == "column" and not col_start:
        col_start = i
        continue

    if not col_start:
      errors.append(f"No columns for {obj_type}.\n"
                    f"'{name}' in {path}:\n'{line}'\n")
      return None, errors

    return (
        TableViewDocs(name, comment[:col_start], comment[col_start:], path),
        errors,
    )

  def check_comment(self) -> List[str]:
    errors = validate_desc(self)
    errors += validate_columns(self)
    return errors


# Stores documentation for CREATE_FUNCTION with comment split into segments.
class FunctionDocs:

  def __init__(
      self,
      path: str,
      data_from_sql: dict,
      module: str,
      name: str,
      desc: str,
      args: List[str],
      ret: List[str],
  ):
    self.path = path
    self.data_from_sql = data_from_sql
    self.module = module
    self.name = name
    self.desc = desc
    self.args = args
    self.ret = ret

  # Contructs new FunctionDocs from whole comment, by splitting it on typed
  # lines. Returns None for improperly structured schemas.
  @staticmethod
  def create_from_comment(path: str, comment: List[str], module: str,
                          matches: tuple) -> tuple["FunctionDocs", List[str]]:
    name, args, ret, sql = matches

    # Ignore internal functions.
    if re.match(r"^INTERNAL_.*", name):
      return None, []

    errors = validate_name(name, module, upper=True)
    start_args, start_ret = None, None

    # Splits code into segments by finding beginning of args and ret segments.
    for i, line in enumerate(comment):
      # Ignore only '--' line.
      if line == "--":
        continue

      m = re.match(typed_comment_pattern(), line)

      # Ignore untyped lines
      if not m:
        continue

      line_type = m.group(1)
      if line_type == "arg" and not start_args:
        start_args = i
        continue

      if line_type == "ret" and not start_ret:
        start_ret = i
        continue

    if not start_ret or not start_args:
      errors.append(f"Function requires 'arg' and 'ret' comments.\n"
                    f"'{name}' in {path}:\n'{line}'\n")
      return None, errors

    args_dict, parse_errors = parse_args(args)
    data_from_sql = {'name': name, 'args': args_dict, 'ret': ret, 'sql': sql}
    return (
        FunctionDocs(
            path,
            data_from_sql,
            module,
            name,
            comment[:start_args],
            comment[start_args:start_ret],
            comment[start_ret:],
        ),
        errors + parse_errors,
    )

  def check_comment(self) -> List[str]:
    errors = validate_desc(self)
    errors += validate_args(self)
    errors += validate_ret(self)
    return errors


# Stores documentation for CREATE_VIEW_FUNCTION with comment split into
# segments.
class ViewFunctionDocs:

  def __init__(
      self,
      path: str,
      data_from_sql: str,
      module: str,
      name: str,
      desc: List[str],
      args: List[str],
      columns: List[str],
  ):
    self.path = path
    self.data_from_sql = data_from_sql
    self.module = module
    self.name = name
    self.desc = desc
    self.args = args
    self.columns = columns

  # Contructs new ViewFunctionDocs from whole comment, by splitting it on typed
  # lines. Returns None for improperly structured schemas.
  @staticmethod
  def create_from_comment(path: str, comment: List[str], module: str,
                          matches: tuple[str]
                         ) -> tuple["ViewFunctionDocs", List[str]]:
    name, args, columns, sql = matches

    # Ignore internal functions.
    if re.match(r"^INTERNAL_.*", name):
      return None, []

    errors = validate_name(name, module, upper=True)
    start_args, start_cols = None, None

    # Splits code into segments by finding beginning of args and cols segments.
    for i, line in enumerate(comment):
      # Ignore only '--' line.
      if line == "--":
        continue

      m = re.match(typed_comment_pattern(), line)

      # Ignore untyped lines
      if not m:
        continue

      line_type = m.group(1)
      if line_type == "arg" and not start_args:
        start_args = i
        continue

      if line_type == "column" and not start_cols:
        start_cols = i
        continue

    if not start_cols or not start_args:
      errors.append(f"Function requires 'arg' and 'column' comments.\n"
                    f"'{name}' in {path}:\n'{line}'\n")
      return None, errors

    args_dict, parse_errors = parse_args(args)
    errors += parse_errors

    cols_dict, parse_errors = parse_args(columns)
    errors += parse_errors

    data_from_sql = dict(name=name, args=args_dict, columns=cols_dict, sql=sql)
    return (
        ViewFunctionDocs(
            path,
            data_from_sql,
            module,
            name,
            comment[:start_args],
            comment[start_args:start_cols],
            comment[start_cols:],
        ),
        errors,
    )

  def check_comment(self) -> List[str]:
    errors = validate_desc(self)
    errors += validate_args(self)
    errors += validate_columns(self, use_data_from_sql=True)
    return errors


# Whether the name starts with module_name.
def validate_name(name: str, module: str, upper: bool = False) -> List[str]:
  module_pattern = f"^{module}_.*"
  if upper:
    module_pattern = module_pattern.upper()
  starts_with_module_name = re.match(module_pattern, name)
  if module == "common":
    if starts_with_module_name:
      return [(f"Invalid name in module {name}. "
               f"In module 'common' the name shouldn't "
               f"start with '{module_pattern}'.\n")]
  else:
    if not starts_with_module_name:
      return [(f"Invalid name in module {name}. "
               f"Name has to begin with '{module_pattern}'.\n")]
  return []


# Whether the only typed comment in provided comment segment is of type
# `comment_type`.
def validate_typed_comment(
    comment_segment: str,
    comment_type: str,
    docs: Union["TableViewDocs", "FunctionDocs", "ViewFunctionDocs"],
) -> List[str]:
  for line in comment_segment:
    # Ignore only '--' line.
    if line == "--":
      continue

    m = re.match(typed_comment_pattern(), line)

    # Ignore untyped lines
    if not m:
      continue

    line_type = m.group(1)

    if line_type != comment_type:
      return [(
          f"Wrong comment type. Expected '{comment_type}', got '{line_type}'.\n"
          f"'{docs.name}' in {docs.path}:\n'{line}'\n")]
  return []


# Whether comment segment with description of the object contains content.
def validate_desc(
    docs: Union["TableViewDocs", "FunctionDocs", "ViewFunctionDocs"]
) -> List[str]:
  for line in docs.desc:
    if line == "--":
      continue
    return []
  return [(f"Missing documentation for {docs.name}\n"
           f"'{docs.name}' in {docs.path}:\n'{line}'\n")]


# Whether comment segment about columns contain proper schema. Can be matched
# against parsed SQL data by setting `use_data_from_sql`.
def validate_columns(docs: Union["TableViewDocs", "ViewFunctionDocs"],
                     use_data_from_sql=False) -> List[str]:
  errors = validate_typed_comment(docs.columns, "column", docs)

  if errors:
    return errors

  if use_data_from_sql:
    cols_from_sql = docs.data_from_sql["columns"]

  for line in docs.columns:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @column"):
      continue

    # Look for '-- @column' line as a column description
    m = re.match(column_pattern(), line)
    if not m:
      errors.append(f"Wrong column description.\n"
                    f"'{docs.name}' in {docs.path}:\n'{line}'\n")
      continue

    if not use_data_from_sql:
      return errors

    col_name = m.group(1)
    if col_name not in cols_from_sql:
      errors.append(f"There is no argument '{col_name}' specified in code.\n"
                    f"'{docs.name}' in {docs.path}:\n'{line}'\n")
      continue

    cols_from_sql.pop(col_name)

  if not use_data_from_sql:
    errors.append(f"Missing columns for {docs.name}\n{docs.path}\n")
    return errors

  if not cols_from_sql:
    return errors

  errors.append(
      f"Missing documentation of columns: {list(cols_from_sql.keys())}.\n"
      f"'{docs.name}' in {docs.path}:\n")
  return errors


# Whether comment segment about columns contain proper schema. Matches against
# parsed SQL data.
def validate_args(docs: "FunctionDocs") -> List[str]:
  errors = validate_typed_comment(docs.args, "arg", docs)

  if errors:
    return errors

  args_from_sql = docs.data_from_sql["args"]
  for line in docs.args:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @"):
      continue

    m = re.match(args_pattern(), line)
    if m is None:
      errors.append("The arg docs formatting is wrong. It should be:\n"
                    "-- @arg [a-z_]* [A-Z]* {desc}\n"
                    f"'{docs.name}' in {docs.path}:\n'{line}'\n")
      return errors

    arg_name, arg_type = m.group(1), m.group(2)
    if arg_name not in args_from_sql:
      errors.append(f"There is not argument '{arg_name}' specified in code.\n"
                    f"'{docs.name}' in {docs.path}:\n'{line}'\n")
      continue

    arg_type_from_sql = args_from_sql.pop(arg_name)
    if arg_type != arg_type_from_sql:
      errors.append(f"In the code, the type of '{arg_name}' is "
                    f"'{arg_type_from_sql}', but according to the docs "
                    f"it is '{arg_type}'.\n"
                    f"'{docs.name}' in {docs.path}:\n'{line}'\n")

  if not args_from_sql:
    return errors

  errors.append(
      f"Missing documentation of args: {list(args_from_sql.keys())}.\n"
      f"{docs.path}\n")
  return errors


# Whether comment segment about return contain proper schema. Matches against
# parsed SQL data.
def validate_ret(docs: "FunctionDocs") -> List[str]:
  errors = validate_typed_comment(docs.ret, "ret", docs)
  if errors:
    return errors

  ret_type_from_sql = docs.data_from_sql["ret"]

  for line in docs.ret:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @ret"):
      continue

    m = re.match(function_return_pattern(), line)
    if m is None:
      return [("The return docs formatting is wrong. It should be:\n"
               "-- @ret [A-Z]* {desc}\n"
               f"'{docs.name}' in {docs.path}:\n'{line}'\n")]
    docs_ret_type = m.group(1)
    if ret_type_from_sql != docs_ret_type:
      return [(f"The return type in docs is '{docs_ret_type}', "
               f"but it is {ret_type_from_sql} in code.\n"
               f"'{docs.name}' in {docs.path}:\n'{line}'\n")]
    return []


# Parses string with multiple arguments with type separated by comma into dict.
def parse_args(args_str: str) -> tuple[dict, List[str]]:
  errors = []
  args = {}
  for arg_str in args_str.split(","):
    m = re.match(arg_str_pattern(), arg_str)
    if m is None:
      errors.append(f"Wrong arguments formatting for '{arg_str}'\n")
      continue
    args[m.group(1)] = m.group(2)
  return args, errors


# After matching file to pattern, fetches and validates related documentation.
def validate_docs_for_sql_object_type(path: str, module: str, sql: str,
                                      pattern: str, docs_object: type):
  errors = []
  line_id_to_match = match_pattern(pattern, sql)
  lines = sql.split("\n")
  for line_id, matches in line_id_to_match.items():
    # Fetch comment by looking at lines over beginning of match in reverse
    # order.
    comment = fetch_comment(lines[line_id - 1::-1])
    docs, obj_errors = docs_object.create_from_comment(path, comment, module,
                                                       matches)
    errors += obj_errors
    if docs:
      errors += docs.check_comment()

  return errors


def check(path: str):
  errors = []

  # Get module name
  module_name = path.split("/stdlib/")[-1].split("/")[0]

  with open(path) as f:
    sql = f.read()

  errors += validate_docs_for_sql_object_type(path, module_name, sql,
                                              create_table_view_pattern(),
                                              TableViewDocs)
  errors += validate_docs_for_sql_object_type(path, module_name, sql,
                                              create_function_pattern(),
                                              FunctionDocs)
  errors += validate_docs_for_sql_object_type(path, module_name, sql,
                                              create_view_function_pattern(),
                                              ViewFunctionDocs)
  return errors


def main():
  errors = []
  metrics_sources = os.path.join(ROOT_DIR, "src", "trace_processor", "stdlib")
  for root, _, files in os.walk(metrics_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if path.endswith(".sql"):
        errors += check(path)
  sys.stderr.write("\n\n".join(errors))
  return 0 if not errors else 1


if __name__ == "__main__":
  sys.exit(main())
