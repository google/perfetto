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
from typing import Union, List, Tuple, Dict
from dataclasses import dataclass

from python.generators.stdlib_docs.utils import *
from python.generators.stdlib_docs.validate import *
from python.generators.stdlib_docs.parse import *

CommentLines = List[str]
AnyDocs = Union['TableViewDocs', 'FunctionDocs', 'ViewFunctionDocs']


# Stores documentation for CREATE {TABLE|VIEW} with comment split into
# segments.
@dataclass
class TableViewDocs:
  name: str
  obj_type: str
  desc: CommentLines
  columns: CommentLines
  path: str

  # Contructs new TableViewDocs from the entire comment, by splitting it on
  # typed lines. Returns None for improperly structured schemas.
  @staticmethod
  def create_from_comment(path: str, comment: CommentLines, module: str,
                          matches: Tuple) -> Tuple['TableViewDocs', Errors]:
    obj_type, name = matches[:2]

    # Ignore internal tables and views.
    if re.match(r"^internal_.*", name):
      return None, []

    errors = validate_name(name, module)
    col_start = None
    has_desc = False

    # Splits code into segments by finding beginning of column segment.
    for i, line in enumerate(comment):
      # Ignore only '--' line.
      if line == "--":
        continue

      m = re.match(Pattern['typed_line'], line)

      # Ignore untyped lines
      if not m:
        if not col_start:
          has_desc = True
        continue

      line_type = m.group(1)
      if line_type == "column" and not col_start:
        col_start = i
        continue

    if not has_desc:
      errors.append(f"No description for {obj_type}: '{name}' in {path}'\n")
      return None, errors

    if not col_start:
      errors.append(f"No columns for {obj_type}: '{name}' in {path}'\n")
      return None, errors

    return (
        TableViewDocs(name, obj_type, comment[:col_start], comment[col_start:],
                      path),
        errors,
    )

  def check_comment(self) -> Errors:
    return validate_columns(self)

  def parse_comment(self) -> dict:
    return {
        'name': self.name,
        'type': self.obj_type,
        'desc': parse_desc(self),
        'cols': parse_columns(self)
    }


# Stores documentation for create_function with comment split into segments.
class FunctionDocs:

  def __init__(
      self,
      path: str,
      data_from_sql: dict,
      module: str,
      name: str,
      desc: str,
      args: CommentLines,
      ret: CommentLines,
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
  def create_from_comment(path: str, comment: CommentLines, module: str,
                          matches: Tuple) -> Tuple['FunctionDocs', Errors]:
    name, args, ret, sql = matches

    # Ignore internal functions.
    if re.match(r"^INTERNAL_.*", name):
      return None, []

    errors = validate_name(name, module, upper=True)
    has_desc, start_args, start_ret = False, None, None

    args_dict, parse_errors = parse_args_str(args)
    errors += parse_errors

    # Splits code into segments by finding beginning of args and ret segments.
    for i, line in enumerate(comment):
      # Ignore only '--' line.
      if line == "--":
        continue

      m = re.match(Pattern['typed_line'], line)

      # Ignore untyped lines
      if not m:
        if not start_args:
          has_desc = True
        continue

      line_type = m.group(1)
      if line_type == "arg" and not start_args:
        start_args = i
        continue

      if line_type == "ret" and not start_ret:
        start_ret = i
        continue

    if not has_desc:
      errors.append(f"No description for '{name}' in {path}'\n")
      return None, errors

    if not start_ret or (args_dict and not start_args):
      errors.append(f"Function requires 'arg' and 'ret' comments.\n"
                    f"'{name}' in {path}\n")
      return None, errors

    if not args_dict:
      start_args = start_ret

    data_from_sql = {'name': name, 'args': args_dict, 'ret': ret, 'sql': sql}
    return (
        FunctionDocs(
            path,
            data_from_sql,
            module,
            name,
            comment[:start_args],
            comment[start_args:start_ret] if args_dict else None,
            comment[start_ret:],
        ),
        errors,
    )

  def check_comment(self) -> Errors:
    errors = validate_args(self)
    errors += validate_ret(self)
    return errors

  def parse_comment(self) -> dict:
    ret_type, ret_desc = parse_ret(self)
    return {
        'name': self.name,
        'desc': parse_desc(self),
        'args': parse_args(self),
        'return_type': ret_type,
        'return_desc': ret_desc
    }


# Stores documentation for create_view_function with comment split into
# segments.
class ViewFunctionDocs:

  def __init__(
      self,
      path: str,
      data_from_sql: str,
      module: str,
      name: str,
      desc: CommentLines,
      args: CommentLines,
      columns: CommentLines,
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
  def create_from_comment(path: str, comment: CommentLines, module: str,
                          matches: Tuple) -> Tuple['ViewFunctionDocs', Errors]:
    name, args, columns, sql = matches

    # Ignore internal functions.
    if re.match(r"^INTERNAL_.*", name):
      return None, []

    errors = validate_name(name, module, upper=True)
    args_dict, parse_errors = parse_args_str(args)
    errors += parse_errors
    has_desc, start_args, start_cols = False, None, None

    # Splits code into segments by finding beginning of args and cols segments.
    for i, line in enumerate(comment):
      # Ignore only '--' line.
      if line == "--":
        continue

      m = re.match(Pattern['typed_line'], line)

      # Ignore untyped lines
      if not m:
        if not start_args:
          has_desc = True
        continue

      line_type = m.group(1)
      if line_type == "arg" and not start_args:
        start_args = i
        continue

      if line_type == "column" and not start_cols:
        start_cols = i
        continue

    if not has_desc:
      errors.append(f"No description for '{name}' in {path}'\n")
      return None, errors

    if not start_cols or (args_dict and not start_args):
      errors.append(f"Function requires 'arg' and 'column' comments.\n"
                    f"'{name}' in {path}\n")
      return None, errors

    if not args_dict:
      start_args = start_cols

    cols_dict, parse_errors = parse_args_str(columns)
    errors += parse_errors

    data_from_sql = dict(name=name, args=args_dict, columns=cols_dict, sql=sql)
    return (
        ViewFunctionDocs(
            path,
            data_from_sql,
            module,
            name,
            comment[:start_args],
            comment[start_args:start_cols] if args_dict else None,
            comment[start_cols:],
        ),
        errors,
    )

  def check_comment(self) -> Errors:
    errors = validate_args(self)
    errors += validate_columns(self, use_data_from_sql=True)
    return errors

  def parse_comment(self) -> dict:
    return {
        'name': self.name,
        'desc': parse_desc(self),
        'args': parse_args(self),
        'cols': parse_columns(self)
    }


# Reads the provided SQL and, if possible, generates a dictionary with data
# from documentation together with errors from validation of the schema.
def parse_file_to_dict(path: str, sql: str) -> Tuple[Dict[str, any], Errors]:
  if sys.platform.startswith('win'):
    path = path.replace("\\", "/")

  # Get module name
  module_name = path.split("/stdlib/")[-1].split("/")[0]

  imports, import_errors = parse_typed_docs(path, module_name, sql,
                                            Pattern['create_table_view'],
                                            TableViewDocs)
  functions, function_errors = parse_typed_docs(path, module_name, sql,
                                                Pattern['create_function'],
                                                FunctionDocs)
  view_functions, view_function_errors = parse_typed_docs(
      path, module_name, sql, Pattern['create_view_function'], ViewFunctionDocs)

  errors = import_errors + function_errors + view_function_errors

  if errors:
    sys.stderr.write("\n\n".join(errors))

  return ({
      'imports': [imp.parse_comment() for imp in imports if imp],
      'functions': [fun.parse_comment() for fun in functions if fun],
      'view_functions': [
          view_fun.parse_comment() for view_fun in view_functions if view_fun
      ]
  }, errors)
