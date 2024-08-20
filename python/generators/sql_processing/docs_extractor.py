#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the 'License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an 'AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from dataclasses import dataclass
from re import Match
from typing import List, Optional, Tuple

from python.generators.sql_processing.utils import ObjKind
from python.generators.sql_processing.utils import extract_comment
from python.generators.sql_processing.utils import match_pattern
from python.generators.sql_processing.utils import PATTERN_BY_KIND


class DocsExtractor:
  """Extracts documentation for views/tables/functions from SQL."""
  path: str
  module_name: str
  sql: str

  @dataclass
  class Annotation:
    key: str
    value: str

  @dataclass
  class Extract:
    """Extracted documentation for a single view/table/function."""
    obj_kind: ObjKind
    obj_match: Match

    description: str
    annotations: List['DocsExtractor.Annotation']

  def __init__(self, path: str, module_name: str, sql: str):
    self.path = path
    self.module_name = module_name
    self.sql = sql

    self.sql_lines = sql.split("\n")
    self.errors = []

  def extract(self) -> List[Extract]:
    extracted = []
    extracted += self._extract_for_kind(ObjKind.table_view)
    extracted += self._extract_for_kind(ObjKind.function)
    extracted += self._extract_for_kind(ObjKind.table_function)
    extracted += self._extract_for_kind(ObjKind.macro)
    extracted += self._extract_for_kind(ObjKind.include)
    return extracted

  def _extract_for_kind(self, kind: ObjKind) -> List[Extract]:
    line_number_to_matches = match_pattern(PATTERN_BY_KIND[kind], self.sql)
    extracts = []
    for line_number, match in sorted(list(line_number_to_matches.items())):
      comment_lines = extract_comment(self.sql_lines, line_number)
      e = self._extract_from_comment(kind, match, comment_lines)
      if e:
        extracts.append(e)
    return extracts

  def _extract_from_comment(self, kind: ObjKind, match: Match,
                            comment_lines: List[str]) -> Optional[Extract]:
    extract = DocsExtractor.Extract(kind, match, '', [])
    for line in comment_lines:
      assert line.startswith('--')

      # Remove the comment.
      comment_stripped = line.lstrip('--')
      stripped = comment_stripped.lstrip()

      # Check if the line is an annotation.
      if not stripped.startswith('@'):
        # We are not in annotation: if we haven't seen an annotation yet, we
        # must be still be parsing the description. Just add to that
        if not extract.annotations:
          extract.description += comment_stripped + "\n"
          continue

        # Otherwise, add to the latest annotation.
        extract.annotations[-1].value += " " + stripped
        continue

      # This line is an annotation: find its name and add a new entry
      annotation, rest = stripped.split(' ', 1)
      extract.annotations.append(DocsExtractor.Annotation(annotation, rest))
    return extract
