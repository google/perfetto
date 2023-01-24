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

import re
from typing import Union, List

from python.generators.stdlib_docs import stdlib
from python.generators.stdlib_docs.utils import Pattern, Errors


# Whether the only typed comment in provided comment segment is of type
# `comment_type`.
def validate_typed_comment(
    comment_segment: str,
    comment_type: str,
    docs: 'stdlib.AnyDocs',
) -> Errors:
  for line in comment_segment:
    # Ignore only '--' line.
    if line == "--":
      continue

    m = re.match(Pattern['typed_line'], line)

    # Ignore untyped lines
    if not m:
      continue

    line_type = m.group(1)

    if line_type != comment_type:
      return [(
          f"Wrong comment type. Expected '{comment_type}', got '{line_type}'.\n"
          f"'{docs.name}' in {docs.path}:\n'{line}'\n")]
  return []


# Whether comment segment about columns contain proper schema. Can be matched
# against parsed SQL data by setting `use_data_from_sql`.
def validate_columns(
    docs: Union['stdlib.TableViewDocs', 'stdlib.ViewFunctionDocs'],
    use_data_from_sql=False) -> Errors:
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
    m = re.match(Pattern['column'], line)
    if not m:
      errors.append(f"Wrong column description.\n"
                    f"'{docs.name}' in {docs.path}:\n'{line}'\n")
      continue

    if not m.group(2).strip():
      errors.append(f"No description for column '{m.group(1)}'.\n"
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
def validate_args(docs: Union['stdlib.FunctionDocs', 'stdlib.ViewFunctionDocs']
                 ) -> Errors:
  if not docs.args:
    return []

  errors = validate_typed_comment(docs.args, "arg", docs)

  if errors:
    return errors

  args_from_sql = docs.data_from_sql["args"]
  for line in docs.args:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @"):
      continue

    m = re.match(Pattern['args'], line)
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
      f"'{docs.name}' in {docs.path}\n")
  return errors


# Whether comment segment about return contain proper schema. Matches against
# parsed SQL data.
def validate_ret(docs: "stdlib.FunctionDocs") -> Errors:
  errors = validate_typed_comment(docs.ret, "ret", docs)
  if errors:
    return errors

  ret_type_from_sql = docs.data_from_sql["ret"]

  for line in docs.ret:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @ret"):
      continue

    m = re.match(Pattern['return_arg'], line)
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
