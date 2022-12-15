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

from typing import List
import re

LOWER_NAME = r'[a-z_\d]*'
UPPER_NAME = r'[A-Z_\d]*'
ANY_WORDS = r'[A-Za-z_\d, ]*'
TYPE = r'[A-Z]*'
SQL = r'[\s\S]*?'


def create_table_view_pattern() -> str:
  return (
      # Match create table/view and catch type
      r'CREATE (?:VIRTUAL )?(TABLE|VIEW)?(?:IF NOT EXISTS)?\s*'
      # Catch the name
      fr'({LOWER_NAME})\s*(?:AS|USING)?.*')


def create_function_pattern() -> str:
  return (r"SELECT\s*CREATE_FUNCTION\(\s*"
          # Function name: we are matching everything [A-Z]* between ' and ).
          fr"'({UPPER_NAME})\s*\("
          # Args: anything before closing bracket with '.
          fr"({ANY_WORDS})\)',\s*"
          # Type: [A-Z]* between two '.
          fr"'({TYPE})',\s*"
          # Sql: Anything between ' and ');. We are catching \'.
          fr"'({SQL})'\s*\);")


def create_view_function_pattern() -> str:
  return (r"SELECT\s*CREATE_VIEW_FUNCTION\(\s*"
          # Function name: we are matching everything [A-Z]* between ' and ).
          fr"'({UPPER_NAME})\s*\("
          # Args: anything before closing bracket with '.
          fr"({ANY_WORDS})\)',\s*"
          # Return columns: anything between two '.
          fr"'({ANY_WORDS})',\s*"
          # Sql: Anything between ' and ');. We are catching \'.
          fr"'({SQL})'\s*\);")


def column_pattern() -> str:
  return fr'^-- @column\s*({LOWER_NAME})\s*(.*)'


def arg_str_pattern() -> str:
  return fr"\s*({LOWER_NAME})\s*({TYPE})\s*"


def args_pattern() -> str:
  return fr'^-- @arg{arg_str_pattern()}(.*)'


def function_return_pattern() -> str:
  return fr"^-- @ret ({TYPE})\s*(.*)"


def typed_comment_pattern() -> str:
  return fr'^-- @([a-z]*)'


def fetch_comment(lines_reversed: List[str]) -> List[str]:
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
