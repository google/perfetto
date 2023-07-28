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

from enum import Enum
import re
from typing import Dict, List

NAME = r'[a-zA-Z_\d]+'
ANY_WORDS = r'[^\s].*'
ANY_NON_QUOTE = r'[^\']*.*'
TYPE = r'[A-Z]+'
SQL = r'[\s\S]*?'
WS = r'\s*'

CREATE_TABLE_VIEW_PATTERN = (
    # Match create table/view and catch type
    fr'CREATE{WS}(?:VIRTUAL )?{WS}(TABLE|VIEW){WS}(?:IF NOT EXISTS)?{WS}'
    # Catch the name
    fr'{WS}({NAME}){WS}(?:AS|USING)?{WS}.*')

CREATE_FUNCTION_PATTERN = (
    # Function name: we are matching everything [A-Z]* between ' and ).
    fr"CREATE{WS}PERFETTO{WS}FUNCTION{WS}({NAME}){WS}"
    # Args: anything before closing bracket.
    fr"{WS}\({WS}({ANY_WORDS}){WS}\){WS}"
    # Type: [A-Z]* between two '.
    fr"{WS}RETURNS{WS}({TYPE}){WS}AS{WS}"
    # Sql: Anything between ' and ');. We are catching \'.
    fr"{WS}({SQL});")

CREATE_VIEW_FUNCTION_PATTERN = (
    fr"SELECT{WS}CREATE_VIEW_FUNCTION\({WS}"
    # Function name: we are matching everything [A-Z]* between ' and ).
    fr"{WS}'{WS}({NAME}){WS}\({WS}"
    # Args: anything before closing bracket with '.
    fr"{WS}({ANY_WORDS}){WS}\){WS}'{WS},{WS}"
    # Return columns: anything between two '.
    fr"'{WS}({ANY_NON_QUOTE}){WS}',{WS}"
    # Sql: Anything between ' and ');. We are catching \'.
    fr"{WS}'{WS}({SQL}){WS}'{WS}\){WS};")


class ObjKind(str, Enum):
  table_view = 'table_view'
  function = 'function'
  view_function = 'view_function'


PATTERN_BY_KIND = {
    ObjKind.table_view: CREATE_TABLE_VIEW_PATTERN,
    ObjKind.function: CREATE_FUNCTION_PATTERN,
    ObjKind.view_function: CREATE_VIEW_FUNCTION_PATTERN,
}

COLUMN_ANNOTATION_PATTERN = fr'^\s*({NAME})\s*({ANY_WORDS})'

NAME_AND_TYPE_PATTERN = fr'\s*({NAME})\s+({TYPE})\s*'

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
