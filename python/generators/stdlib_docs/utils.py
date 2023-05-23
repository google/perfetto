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
from typing import Dict, List

from python.generators.stdlib_docs.types import ObjKind

LOWER_NAME = r'[a-z_\d]+'
UPPER_NAME = r'[A-Z_\d]+'
ANY_WORDS = r'[^\s].*'
TYPE = r'[A-Z]+'
SQL = r'[\s\S]*?'

CREATE_TABLE_VIEW_PATTERN = (
    # Match create table/view and catch type
    r'CREATE (?:VIRTUAL )?(TABLE|VIEW)?(?:IF NOT EXISTS)?\s*'
    # Catch the name
    fr'({LOWER_NAME})\s*(?:AS|USING)?.*')

CREATE_FUNCTION_PATTERN = (
    r"SELECT\s*CREATE_FUNCTION\(\s*"
    # Function name: we are matching everything [A-Z]* between ' and ).
    fr"'\s*({UPPER_NAME})\s*\("
    # Args: anything before closing bracket with '.
    fr"({ANY_WORDS})\)',\s*"
    # Type: [A-Z]* between two '.
    fr"'({TYPE})',\s*"
    # Sql: Anything between ' and ');. We are catching \'.
    fr"'({SQL})'\s*\);")

CREATE_VIEW_FUNCTION_PATTERN = (
    r"SELECT\s*CREATE_VIEW_FUNCTION\(\s*"
    # Function name: we are matching everything [A-Z]* between ' and ).
    fr"'({UPPER_NAME})\s*\("
    # Args: anything before closing bracket with '.
    fr"({ANY_WORDS})\)',\s*"
    # Return columns: anything between two '.
    fr"'\s*({ANY_WORDS})',\s*"
    # Sql: Anything between ' and ');. We are catching \'.
    fr"'({SQL})'\s*\);")

PATTERN_BY_KIND = {
    ObjKind.table_view: CREATE_TABLE_VIEW_PATTERN,
    ObjKind.function: CREATE_FUNCTION_PATTERN,
    ObjKind.view_function: CREATE_VIEW_FUNCTION_PATTERN,
}

COLUMN_ANNOTATION_PATTERN = fr'^\s*({LOWER_NAME})\s*({ANY_WORDS})'

NAME_AND_TYPE_PATTERN = fr'\s*({LOWER_NAME})\s+({TYPE})\s*'

ARG_ANNOTATION_PATTERN = fr'\s*{NAME_AND_TYPE_PATTERN}\s+({ANY_WORDS})'

FUNCTION_RETURN_PATTERN = fr'^\s*({TYPE})\s+({ANY_WORDS})'


# Given a list of lines in a text and the line number, scans backwards to find
# all the comments.
def extract_comment(lines: List[str], line_number: int) -> List[str]:
  comments = []
  for line in lines[line_number - 1::-1]:
    # Break on empty line, as that suggests it is no longer a part of
    # this comment.
    if not line or not line.startswith('--'):
      break
    comments.append(line)

  # Reverse as the above was reversed
  comments.reverse()
  return comments


# Given a regex pattern and a string to match against, returns all the
# matching positions. Specifically, it returns a dictionary from the line
# number of the match to the regex match object.
def match_pattern(pattern: str, file_str: str) -> Dict[int, re.Match]:
  line_number_to_matches = {}
  for match in re.finditer(pattern, file_str):
    line_id = file_str[:match.start()].count('\n')
    line_number_to_matches[line_id] = match.groups()
  return line_number_to_matches
