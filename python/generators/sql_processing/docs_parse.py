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

from abc import ABC
from dataclasses import dataclass
import re
import sys
from typing import Any, Dict, List, Optional, Set, Tuple

from python.generators.sql_processing.docs_extractor import DocsExtractor
from python.generators.sql_processing.utils import ObjKind
from python.generators.sql_processing.utils import ARG_ANNOTATION_PATTERN
from python.generators.sql_processing.utils import NAME_AND_TYPE_PATTERN
from python.generators.sql_processing.utils import FUNCTION_RETURN_PATTERN
from python.generators.sql_processing.utils import COLUMN_ANNOTATION_PATTERN


def is_internal(name: str) -> bool:
  return re.match(r'^internal_.*', name, re.IGNORECASE) is not None


def is_snake_case(s: str) -> bool:
  """Returns true if the string is snake_case."""
  return re.fullmatch(r'^[a-z_0-9]*$', s) is not None


class AbstractDocParser(ABC):

  @dataclass
  class Column:
    pass

  def __init__(self, path: str, module: str):
    self.path = path
    self.module = module
    self.name = None
    self.errors = []

  def _parse_name(self, upper: bool = False):
    assert self.name
    assert isinstance(self.name, str)
    module_pattern = f"^{self.module}_.*"
    if upper:
      module_pattern = module_pattern.upper()
    starts_with_module_name = re.match(module_pattern, self.name, re.IGNORECASE)
    if self.module == "common":
      if starts_with_module_name:
        self._error('Names of tables/views/functions in the "common" module '
                    f'should not start with {module_pattern}')
      return self.name
    if not starts_with_module_name:
      self._error('Names of tables/views/functions should be prefixed with the '
                  f'module name (i.e. should start with {module_pattern})')
    return self.name.strip()

  def _parse_desc_not_empty(self, desc: str):
    if not desc:
      self._error('Description of the table/view/function is missing')
    return desc.strip()

  def _validate_only_contains_annotations(self,
                                          ans: List[DocsExtractor.Annotation],
                                          ans_types: Set[str]):
    used_ans_types = set(a.key for a in ans)
    for type in used_ans_types.difference(ans_types):
      self._error(f'Unknown documentation annotation {type}')

  def _parse_columns(self, ans: List[DocsExtractor.Annotation],
                     sql_cols_str: str) -> Dict[str, str]:
    cols = {}
    for t in ans:
      if t.key != '@column':
        continue
      m = re.match(COLUMN_ANNOTATION_PATTERN, t.value)
      if not m:
        self._error(f'@column annotation value {t.value} does not match '
                    f'pattern {COLUMN_ANNOTATION_PATTERN}')
        continue
      cols[m.group(1)] = m.group(2).strip()

    sql_cols = self._parse_name_and_types_str(sql_cols_str)
    if sql_cols:
      for col in set(cols.keys()).difference(sql_cols.keys()):
        self._error(f'@column "{col}" documented but does not exist in '
                    'function definition')
      for col in set(sql_cols.keys()).difference(cols):
        self._error(f'Column "{col}" defined in SQL but is not documented with '
                    '@column')
    return cols

  def _parse_args(self, ans: List[DocsExtractor.Annotation],
                  sql_args_str: str) -> Dict[str, Any]:
    args = {}
    for an in ans:
      if an.key != '@arg':
        continue
      m = re.match(ARG_ANNOTATION_PATTERN, an.value)
      if m is None:
        self._error(f'Expected arg documentation "{an.value}" to match pattern '
                    f'{ARG_ANNOTATION_PATTERN}')
        continue
      args[m.group(1)] = {'type': m.group(2), 'desc': m.group(3).strip()}

    sql_args = self._parse_name_and_types_str(sql_args_str)
    if sql_args:
      for col in set(args.keys()).difference(sql_args.keys()):
        self._error(f'Arg "{col}" documented with @arg but does not exist '
                    'in function definition')
      for arg in set(sql_args.keys()).difference(args.keys()):
        self._error(f'Arg "{arg}" defined in SQL but is not documented with '
                    '@arg')
    return args

  def _parse_ret(self, ans: List[DocsExtractor.Annotation],
                 sql_ret_type: str) -> Tuple[str, str]:
    rets = [a.value for a in ans if a.key == '@ret']
    if len(rets) != 1:
      self._error('Return value is not documentated with @ret')
      return '', ''

    ret = rets[0]
    m = re.match(FUNCTION_RETURN_PATTERN, ret)
    if not m:
      self._error(
          f'@ret {ret} does not match pattern {FUNCTION_RETURN_PATTERN}')
      return '', ''

    ret_type, ret_desc = m.group(1), m.group(2)
    if ret_type != sql_ret_type:
      self._error(
          f'@ret {ret_type} does not match SQL return type {sql_ret_type}')
      return '', ''
    return ret_type, ret_desc.strip()

  def _parse_name_and_types_str(self, args_str: str) -> Dict[str, str]:
    if not args_str:
      return {}

    args = {}
    for arg_str in args_str.split(","):
      m = re.match(NAME_AND_TYPE_PATTERN, arg_str)
      if m is None:
        self._error(f'Expected "{arg_str}" to match pattern '
                    f'{NAME_AND_TYPE_PATTERN}')
        continue
      args[m.group(1)] = m.group(2).strip()
    return args

  def _error(self, error: str):
    self.errors.append(
        f'Error while parsing documentation for {self.name} in {self.path}: '
        f'{error}')


class TableOrView:
  name: str
  type: str
  desc: str
  cols: Dict[str, str]

  def __init__(self, name, type, desc, cols):
    self.name = name
    self.type = type
    self.desc = desc
    self.cols = cols


class TableViewDocParser(AbstractDocParser):
  """Parses documentation for CREATE TABLE and CREATE VIEW statements."""

  def __init__(self, path: str, module: str):
    super().__init__(path, module)

  def parse(self, doc: DocsExtractor.Extract) -> Optional[TableOrView]:
    assert doc.obj_kind == ObjKind.table_view

    self.name = doc.obj_match[1]
    if is_internal(self.name):
      return None

    self._validate_only_contains_annotations(doc.annotations, {'@column'})
    return TableOrView(
        name=self._parse_name(),
        type=doc.obj_match[0],
        desc=self._parse_desc_not_empty(doc.description),
        cols=self._parse_columns(doc.annotations, ''),
    )


class Function:
  name: str
  desc: str
  args: Dict[str, Any]
  return_type: str
  return_desc: str

  def __init__(self, name, desc, args, return_type, return_desc):
    self.name = name
    self.desc = desc
    self.args = args
    self.return_type = return_type
    self.return_desc = return_desc


class FunctionDocParser(AbstractDocParser):
  """Parses documentation for CREATE_FUNCTION statements."""

  def __init__(self, path: str, module: str):
    super().__init__(path, module)

  def parse(self, doc: DocsExtractor.Extract) -> Optional[Function]:
    self.name, args, ret, _ = doc.obj_match

    # Ignore internal functions.
    if is_internal(self.name):
      return None

    self._validate_only_contains_annotations(doc.annotations, {'@arg', '@ret'})

    ret_type, ret_desc = self._parse_ret(doc.annotations, ret)
    name = self._parse_name()

    if not is_snake_case(name):
      self._error('Function name %s is not snake_case (should be %s) ' %
                  (name, name.casefold()))

    return Function(
        name=name,
        desc=self._parse_desc_not_empty(doc.description),
        args=self._parse_args(doc.annotations, args),
        return_type=ret_type,
        return_desc=ret_desc,
    )


class TableFunction:
  name: str
  desc: str
  cols: Dict[str, str]
  args: Dict[str, Any]

  def __init__(self, name, desc, cols, args):
    self.name = name
    self.desc = desc
    self.cols = cols
    self.args = args


class ViewFunctionDocParser(AbstractDocParser):
  """Parses documentation for CREATE_VIEW_FUNCTION statements."""

  def __init__(self, path: str, module: str):
    super().__init__(path, module)

  def parse(self, doc: DocsExtractor.Extract) -> Optional[TableFunction]:
    self.name, args, columns, _ = doc.obj_match

    # Ignore internal functions.
    if is_internal(self.name):
      return None

    self._validate_only_contains_annotations(doc.annotations,
                                             {'@arg', '@column'})
    name = self._parse_name()

    if not is_snake_case(name):
      self._error('Function name %s is not snake_case (should be %s) ' %
                  (name, name.casefold()))

    return TableFunction(
        name=name,
        desc=self._parse_desc_not_empty(doc.description),
        cols=self._parse_columns(doc.annotations, columns),
        args=self._parse_args(doc.annotations, args),
    )


class ParsedFile:
  errors: List[str] = []
  table_views: List[TableOrView] = []
  functions: List[Function] = []
  table_functions: List[TableFunction] = []

  def __init__(self, errors, table_views, functions, view_functions):
    self.errors = errors
    self.table_views = table_views
    self.functions = functions
    self.table_functions = view_functions


# Reads the provided SQL and, if possible, generates a dictionary with data
# from documentation together with errors from validation of the schema.
def parse_file(path: str, sql: str) -> ParsedFile:
  if sys.platform.startswith('win'):
    path = path.replace('\\', '/')

  # Get module name
  module_name = path.split('/stdlib/')[-1].split('/')[0]

  # Extract all the docs from the SQL.
  extractor = DocsExtractor(path, module_name, sql)
  docs = extractor.extract()
  if extractor.errors:
    return ParsedFile(extractor.errors, [], [], [])

  # Parse the extracted docs.
  errors = []
  table_views = []
  functions = []
  view_functions = []
  for doc in docs:
    if doc.obj_kind == ObjKind.table_view:
      parser = TableViewDocParser(path, module_name)
      res = parser.parse(doc)
      if res:
        table_views.append(res)
      errors += parser.errors
    if doc.obj_kind == ObjKind.function:
      parser = FunctionDocParser(path, module_name)
      res = parser.parse(doc)
      if res:
        functions.append(res)
      errors += parser.errors
    if doc.obj_kind == ObjKind.view_function:
      parser = ViewFunctionDocParser(path, module_name)
      res = parser.parse(doc)
      if res:
        view_functions.append(res)
      errors += parser.errors

  return ParsedFile(errors, table_views, functions, view_functions)
