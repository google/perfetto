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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import re
from typing import List, Tuple

Errors = List[str]
CommentLines = List[str]

LOWER_NAME = r'[a-z_\d]*'
UPPER_NAME = r'[A-Z_\d]*'
ANY_WORDS = r'[A-Za-z_\d, \n]*'
TYPE = r'[A-Z]*'
SQL = r'[\s\S]*?'

Pattern = {
    'create_table_view': (
        # Match create table/view and catch type
        r'CREATE (?:VIRTUAL )?(TABLE|VIEW)?(?:IF NOT EXISTS)?\s*'
        # Catch the name
        fr'({LOWER_NAME})\s*(?:AS|USING)?.*'),
    'create_function': (
        r"SELECT\s*CREATE_FUNCTION\(\s*"
        # Function name: we are matching everything [A-Z]* between ' and ).
        fr"'\s*({UPPER_NAME})\s*\("
        # Args: anything before closing bracket with '.
        fr"({ANY_WORDS})\)',\s*"
        # Type: [A-Z]* between two '.
        fr"'({TYPE})',\s*"
        # Sql: Anything between ' and ');. We are catching \'.
        fr"'({SQL})'\s*\);"),
    'create_view_function': (
        r"SELECT\s*CREATE_VIEW_FUNCTION\(\s*"
        # Function name: we are matching everything [A-Z]* between ' and ).
        fr"'({UPPER_NAME})\s*\("
        # Args: anything before closing bracket with '.
        fr"({ANY_WORDS})\)',\s*"
        # Return columns: anything between two '.
        fr"'\s*({ANY_WORDS})',\s*"
        # Sql: Anything between ' and ');. We are catching \'.
        fr"'({SQL})'\s*\);"),
    'column': fr'^-- @column\s*({LOWER_NAME})\s*({ANY_WORDS})',
    'arg_str': fr"\s*({LOWER_NAME})\s*({TYPE})\s*",
    'args': fr'^-- @arg\s*({LOWER_NAME})\s*({TYPE})\s*(.*)',
    'return_arg': fr"^-- @ret ({TYPE})\s*(.*)",
    'typed_line': fr'^-- @([a-z]*)'
}


def fetch_comment(lines_reversed: CommentLines) -> CommentLines:
  comment_reversed = []
  for line in lines_reversed:
    # Break on empty line, as that suggests it is no longer a part of
    # this comment.
    if not line or not line.startswith('--'):
      break

    # The only  option left is a description, but it has to be after
    # schema columns.
    comment_reversed.append(line)

  comment_reversed.reverse()
  return comment_reversed


def match_pattern(pattern: str, file_str: str) -> dict:
  objects = {}
  for match in re.finditer(pattern, file_str):
    line_id = file_str[:match.start()].count('\n')
    objects[line_id] = match.groups()
  return dict(sorted(objects.items()))


# Whether the name starts with module_name.
def validate_name(name: str, module: str, upper: bool = False) -> Errors:
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


# Parses string with multiple arguments with type separated by comma into dict.
def parse_args_str(args_str: str) -> Tuple[dict, Errors]:
  if not args_str.strip():
    return None, []

  errors = []
  args = {}
  for arg_str in args_str.split(","):
    m = re.match(Pattern['arg_str'], arg_str)
    if m is None:
      errors.append(f"Wrong arguments formatting for '{arg_str}'\n")
      continue
    args[m.group(1)] = m.group(2)
  return args, errors


def get_text(line: str, no_break_line: bool = True) -> str:
  line = line.lstrip('--').strip()
  if not line:
    return '' if no_break_line else '\n'
  return line
