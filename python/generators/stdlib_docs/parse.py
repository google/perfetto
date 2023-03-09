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
from typing import Union, List, Tuple

from python.generators.stdlib_docs import stdlib
from python.generators.stdlib_docs.utils import Errors, Pattern, get_text, fetch_comment, match_pattern


def parse_desc(docs: 'stdlib.AnyDocs') -> str:
  desc_lines = [get_text(line, False) for line in docs.desc]
  return ' '.join(desc_lines).strip('\n').strip()


# Whether comment segment about columns contain proper schema. Can be matched
# against parsed SQL data by setting `use_data_from_sql`.
def parse_columns(docs: Union['stdlib.TableViewDocs', 'stdlib.ViewFunctionDocs']
                 ) -> dict:
  cols = {}
  last_col = None
  last_desc = []
  for line in docs.columns:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @column"):
      last_desc.append(get_text(line))
      continue

    # Look for '-- @column' line as a column description
    m = re.match(Pattern['column'], line)
    if last_col:
      cols[last_col] = ' '.join(last_desc)
    last_col, last_desc = m.group(1), [m.group(2)]

  cols[last_col] = ' '.join(last_desc)
  return cols


def parse_args(docs: "stdlib.FunctionDocs") -> dict:
  if not docs.args:
    return {}

  args = {}
  last_arg, last_desc, last_type = None, [], None
  for line in docs.args:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @arg"):
      last_desc.append(get_text(line))
      continue

    m = re.match(Pattern['args'], line)
    if last_arg:
      args[last_arg] = {'type': last_type, 'desc': ' '.join(last_desc)}
    last_arg, last_type, last_desc = m.group(1), m.group(2), [m.group(3)]

  args[last_arg] = {'type': last_type, 'desc': ' '.join(last_desc)}
  return args


# Whether comment segment about return contain proper schema. Matches against
# parsed SQL data.
def parse_ret(docs: "stdlib.FunctionDocs") -> Tuple[str, str]:
  desc = []
  for line in docs.ret:
    # Ignore only '--' line.
    if line == "--" or not line.startswith("-- @ret"):
      desc.append(get_text(line))

    m = re.match(Pattern['return_arg'], line)
    ret_type, desc = m.group(1), [m.group(2)]
  return (ret_type, ' '.join(desc))


# After matching file to Pattern, fetches and validates related documentation.
def parse_typed_docs(path: str, module: str, sql: str, Pattern: str,
                     docs_object: type
                    ) -> Tuple[List['stdlib.AnyDocs'], Errors]:
  errors = []
  line_id_to_match = match_pattern(Pattern, sql)
  lines = sql.split("\n")
  all_typed_docs = []
  for line_id, matches in line_id_to_match.items():
    # Fetch comment by looking at lines over beginning of match in reverse
    # order.
    comment = fetch_comment(lines[line_id - 1::-1])
    typed_docs, obj_errors = docs_object.create_from_comment(
        path, comment, module, matches)
    errors += obj_errors

    if not typed_docs:
      continue

    errors += typed_docs.check_comment()

    if not errors:
      all_typed_docs.append(typed_docs)

  return all_typed_docs, errors
