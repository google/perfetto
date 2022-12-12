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


def create_table_view_pattern():
  return (
      # Match create table/view and catch type
      r'CREATE (?:VIRTUAL )?(TABLE|VIEW)?(?:IF NOT EXISTS)?\s*'
      # Catch the name
      r'([a-z_\d]*)\s*(?:AS|USING)?.*')


def column_pattern():
  return r'^-- @column[ \t]+(\w+)[ \t]+(.*)'


def create_function_pattern():
  return (r"SELECT\s*CREATE_FUNCTION\(\s*"
          # Function name: we are matching everything [A-Z]* between ' and ).
          r"'([A-Z_]*)\s*\("
          # Args: anything before closing bracket with '.
          r"([A-Za-z_\d, ]*)\)',\s*"
          # Type: [A-Z]* between two '.
          r"'([A-Z]*)',\s*"
          # Sql: Anything between ' and ');. We are catching \'.
          r"'([\s\S]*?)'\s*\);")


def args_pattern():
  return r'^-- @arg\s*([a-z_]*)\s*([A-Z]*)\s*(.*)'


def arg_pattern():
  return r"\s*([a-z_]*)\s*([A-Z]*)\s*"


def function_return_pattern():
  return r"^-- @ret ([A-Z]*)\s*(.*)"


def fetch_comment(lines_reversed):
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


def match_pattern(pattern, file_str):
  objects = {}
  for match in re.finditer(pattern, file_str):
    line_id = file_str[:match.start()].count('\n')
    objects[line_id] = match.groups()
  return dict(sorted(objects.items()))
