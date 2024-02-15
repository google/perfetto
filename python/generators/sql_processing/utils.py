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
import os
from typing import Dict, List

NAME = r'[a-zA-Z_\d\{\}]+'
ANY_WORDS = r'[^\s].*'
ANY_NON_QUOTE = r'[^\']*.*'
TYPE = r'[a-zA-Z]+'
SQL = r'[\s\S]*?'
WS = r'\s*'
COMMENT = r' --[^\n]*\n'
COMMENTS = rf'(?:{COMMENT})*'
ARG = rf'{COMMENTS} {NAME} {TYPE}'
ARG_PATTERN = rf'({COMMENTS}) ({NAME}) ({TYPE})'
ARGS = rf'(?:{ARG})?(?: ,{ARG})*'


# Make the pattern more readable by allowing the use of spaces
# and replace then with a wildcard in a separate step.
# NOTE: two whitespaces next to each other are really bad for performance.
# Take special care to avoid them.
def update_pattern(pattern):
  return pattern.replace(' ', WS)


CREATE_TABLE_VIEW_PATTERN = update_pattern(
    # Match create table/view and catch type
    fr'^CREATE (OR REPLACE)? (VIRTUAL|PERFETTO)?'
    fr' (TABLE|VIEW) (?:IF NOT EXISTS)?'
    # Catch the name and optional schema.
    fr' ({NAME}) (?: \( ({ARGS}) \) )? (?:AS|USING)? .*')

CREATE_TABLE_AS_PATTERN = update_pattern(fr'^CREATE TABLE ({NAME}) AS')

CREATE_VIEW_AS_PATTERN = update_pattern(fr'^CREATE VIEW ({NAME}) AS')

DROP_TABLE_VIEW_PATTERN = update_pattern(fr'^DROP (TABLE|VIEW) IF EXISTS '
                                         fr'({NAME});$')

INCLUDE_ALL_PATTERN = update_pattern(
    fr'^INCLUDE PERFETTO MODULE [a-zA-Z0-9_\.]*\*;')

CREATE_FUNCTION_PATTERN = update_pattern(
    # Function name.
    fr"CREATE (OR REPLACE)? PERFETTO FUNCTION ({NAME}) "
    # Args: anything in the brackets.
    fr" \( ({ARGS}) \)"
    # Type: word after RETURNS.
    fr"({COMMENTS})"
    fr" RETURNS ({TYPE}) AS ")

CREATE_TABLE_FUNCTION_PATTERN = update_pattern(
    fr"CREATE (OR REPLACE)? PERFETTO FUNCTION ({NAME}) "
    # Args: anything in the brackets.
    fr" \( ({ARGS}) \) "
    # Type: table definition after RETURNS.
    fr"({COMMENTS})"
    fr" RETURNS TABLE\( ({ARGS}) \) AS ")

CREATE_MACRO_PATTERN = update_pattern(
    fr"CREATE (OR REPLACE)? PERFETTO MACRO ({NAME}) "
    # Args: anything in the brackets.
    fr" \( ({ARGS}) \) "
    # Type: word after RETURNS.
    fr"({COMMENTS})"
    fr" RETURNS ({TYPE})")

COLUMN_ANNOTATION_PATTERN = update_pattern(fr'^ ({NAME}) ({ANY_WORDS})')

NAME_AND_TYPE_PATTERN = update_pattern(fr' ({NAME})\s+({TYPE}) ')

ARG_ANNOTATION_PATTERN = fr'\s*{NAME_AND_TYPE_PATTERN}\s+({ANY_WORDS})'

ARG_DEFINITION_PATTERN = update_pattern(ARG_PATTERN)

FUNCTION_RETURN_PATTERN = update_pattern(fr'^ ({TYPE})\s+({ANY_WORDS})')

ANY_PATTERN = r'(?:\s|.)*'


class ObjKind(str, Enum):
  table_view = 'table_view'
  function = 'function'
  table_function = 'table_function'
  macro = 'macro'


PATTERN_BY_KIND = {
    ObjKind.table_view: CREATE_TABLE_VIEW_PATTERN,
    ObjKind.function: CREATE_FUNCTION_PATTERN,
    ObjKind.table_function: CREATE_TABLE_FUNCTION_PATTERN,
    ObjKind.macro: CREATE_MACRO_PATTERN
}

ALLOWED_PREFIXES = {
    'counters': 'counter',
    'chrome/util': 'cr',
    'graphs': 'graph',
    'slices': 'slice'
}

# Allows for nonstandard object names.
OBJECT_NAME_ALLOWLIST = {
    'slices/with_context.sql': ['process_slice', 'thread_slice']
}

# Given a regex pattern and a string to match against, returns all the
# matching positions. Specifically, it returns a dictionary from the line
# number of the match to the regex match object.
# Note: this resuts a dict[int, re.Match], but re.Match exists only in later
# versions of python3, prior to that it was _sre.SRE_Match.
def match_pattern(pattern: str, file_str: str) -> Dict[int, object]:
  line_number_to_matches = {}
  for match in re.finditer(pattern, file_str, re.MULTILINE):
    line_id = file_str[:match.start()].count('\n')
    line_number_to_matches[line_id] = match.groups()
  return line_number_to_matches


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


# Given SQL string check whether any of the words is used, and create error
# string if needed.
def check_banned_words(sql: str, path: str) -> List[str]:
  lines = [l.strip() for l in sql.split('\n')]
  errors = []

  # Ban the use of LIKE in non-comment lines.
  for line in lines:
    if line.startswith('--'):
      continue

    if 'like' in line.casefold():
      errors.append(
          'LIKE is banned in trace processor metrics. Prefer GLOB instead.\n'
          f'Offending file: {path}\n')
      continue

    if 'create_function' in line.casefold():
      errors.append('CREATE_FUNCTION is deprecated in trace processor. '
                    'Use CREATE PERFETTO FUNCTION instead.\n'
                    f'Offending file: {path}')

    if 'create_view_function' in line.casefold():
      errors.append(
          'CREATE_VIEW_FUNCTION is deprecated in trace processor. '
          'Use CREATE PERFETTO FUNCTION $name RETURNS TABLE instead.\n'
          f'Offending file: {path}')

    if 'import(' in line.casefold():
      errors.append('SELECT IMPORT is deprecated in trace processor. '
                    'Use INCLUDE PERFETTO MODULE instead.\n'
                    f'Offending file: {path}')

  return errors


# Given SQL string check whether there is (not allowlisted) usage of
# CREATE TABLE {name} AS.
def check_banned_create_table_as(sql: str, filename: str, stdlib_path: str,
                                 allowlist: Dict[str, List[str]]) -> List[str]:
  errors = []
  for _, matches in match_pattern(CREATE_TABLE_AS_PATTERN, sql).items():
    name = matches[0]
    # Normalize paths before checking presence in the allowlist so it will
    # work on Windows for the Chrome stdlib presubmit.
    allowlist_normpath = dict(
        (os.path.normpath(path), tables) for path, tables in allowlist.items())
    allowlist_key = os.path.normpath(filename[len(stdlib_path):])
    if allowlist_key not in allowlist_normpath:
      errors.append(f"CREATE TABLE '{name}' is deprecated. "
                    "Use CREATE PERFETTO TABLE instead.\n"
                    f"Offending file: {filename}\n")
      continue
    if name not in allowlist_normpath[allowlist_key]:
      errors.append(
          f"Table '{name}' uses CREATE TABLE which is deprecated "
          "and this table is not allowlisted. Use CREATE PERFETTO TABLE.\n"
          f"Offending file: {filename}\n")
  return errors


# Given SQL string check whether there is usage of CREATE VIEW {name} AS.
def check_banned_create_view_as(sql: str, filename: str) -> List[str]:
  errors = []
  for _, matches in match_pattern(CREATE_VIEW_AS_PATTERN, sql).items():
    name = matches[0]
    errors.append(f"CREATE VIEW '{name}' is deprecated. "
                  "Use CREATE PERFETTO VIEW instead.\n"
                  f"Offending file: {filename}\n")
  return errors


# Given SQL string check whether there is usage of CREATE VIEW {name} AS.
def check_banned_include_all(sql: str, filename: str) -> List[str]:
  errors = []
  for _, matches in match_pattern(INCLUDE_ALL_PATTERN, sql).items():
    errors.append(
        f"INCLUDE PERFETTO MODULE with wildcards is not allowed in stdlib. "
        f"Import specific modules instead. Offending file: {filename}")
  return errors
