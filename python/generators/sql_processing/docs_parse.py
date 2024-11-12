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
from typing import Dict, List, Optional, Set, NamedTuple

from python.generators.sql_processing.docs_extractor import DocsExtractor
from python.generators.sql_processing.utils import ObjKind

from python.generators.sql_processing.utils import ALLOWED_PREFIXES
from python.generators.sql_processing.utils import OBJECT_NAME_ALLOWLIST

from python.generators.sql_processing.utils import ANY_PATTERN
from python.generators.sql_processing.utils import ARG_DEFINITION_PATTERN


def _is_internal(name: str) -> bool:
  return re.match(r'^_.*', name, re.IGNORECASE) is not None


def _is_snake_case(s: str) -> bool:
  return re.fullmatch(r'^[a-z_0-9]*$', s) is not None


def parse_comment(comment: str) -> str:
  """Parse a SQL comment (i.e. -- Foo\n -- bar.) into a string (i.e. "Foo bar.")."""
  return ' '.join(line.strip().lstrip('--').lstrip()
                  for line in comment.strip().split('\n'))


def get_module_prefix_error(name: str, path: str, module: str) -> Optional[str]:
  """Returns error message if the name is not correct, None otherwise."""
  if module in ["common", "prelude", "deprecated"]:
    if name.startswith(module):
      return (f'Names of tables/views/functions in the "{module}" module '
              f'should not start with {module}')
    return None
  if name.startswith(module):
    # Module prefix is always allowed.
    return None
  allowed_prefixes = [module]
  for (path_prefix, allowed_name_prefixes) in ALLOWED_PREFIXES.items():
    if path.startswith(path_prefix):
      for prefix in allowed_name_prefixes:
        if name.startswith(prefix):
          return None
      allowed_prefixes.extend(allowed_name_prefixes)
    if path in OBJECT_NAME_ALLOWLIST and name in OBJECT_NAME_ALLOWLIST[path]:
      return None
  return (
      f'Names of tables/views/functions at path "{path}" should be prefixed '
      f'with one of following names: {", ".join(allowed_prefixes)}')


class Arg(NamedTuple):
  # TODO(b/307926059): the type is missing on old-style documentation for
  # tables. Make it "str" after stdlib is migrated.
  type: Optional[str]
  description: str


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
    module_prefix_error = get_module_prefix_error(self.name, self.path,
                                                  self.module)
    if module_prefix_error is not None:
      self._error(module_prefix_error)
    return self.name.strip()

  def _parse_desc_not_empty(self, desc: str):
    if not desc:
      self._error('Description of the table/view/function/macro is missing')
    return desc.strip()

  def _parse_columns(self, schema: str) -> Dict[str, Arg]:
    columns = self._parse_args_definition(schema) if schema else {}
    for column in columns:
      if not columns[column].description:
        self._error(f'Column "{column}" is missing a description. Please add a '
                    'comment in front of the column definition')
        continue

    return columns

  def _parse_args(self, sql_args_str: str) -> Dict[str, Arg]:
    args = self._parse_args_definition(sql_args_str)

    for arg in args:
      if not args[arg].description:
        self._error(f'Arg "{arg}" is missing a description. '
                    'Please add a comment in front of the arg definition.')
    return args

  # Parse function argument definition list or a table schema, e.g.
  # arg1 INT, arg2 STRING, including their comments.
  def _parse_args_definition(self, args_str: str) -> Dict[str, Arg]:
    result = {}
    remaining_args = args_str.strip()
    while remaining_args:
      m = re.match(fr'^{ARG_DEFINITION_PATTERN}({ANY_PATTERN})', remaining_args)
      if not m:
        self._error(f'Expected "{args_str}" to correspond to '
                    '"-- Comment\n arg_name TYPE" format '
                    '({ARG_DEFINITION_PATTERN})')
        return result
      groups = m.groups()
      comment = '' if groups[0] is None else parse_comment(groups[0])
      name = groups[-3]
      type = groups[-2]
      result[name] = Arg(type, comment)
      # Strip whitespace and comma and parse the next arg.
      remaining_args = groups[-1].lstrip().lstrip(',').lstrip()

    return result

  def _error(self, error: str):
    self.errors.append(
        f'Error while parsing documentation for "{self.name}" in {self.path}: '
        f'{error}')


class TableOrView:
  name: str
  type: str
  desc: str
  cols: Dict[str, Arg]

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

    or_replace, perfetto_or_virtual, type, self.name, schema = doc.obj_match

    if or_replace is not None:
      self._error(
          f'{type} "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')
      return

    if _is_internal(self.name):
      return None

    if not schema and self.name.lower() != "window":
      self._error(
          f'{type} "{self.name}": schema is missing for a non-internal stdlib'
          f' perfetto table or view')
      return

    if type.lower() == "table" and not perfetto_or_virtual:
      self._error(
          f'{type} "{self.name}": Can only expose CREATE PERFETTO tables')
      return

    is_virtual_table = type.lower() == "table" and perfetto_or_virtual.lower(
    ) == "virtual"
    if is_virtual_table and self.name.lower() != "window":
      self._error(f'{type} "{self.name}": Virtual tables cannot be exposed.')
      return

    return TableOrView(
        name=self._parse_name(),
        type=type,
        desc=self._parse_desc_not_empty(doc.description),
        cols=self._parse_columns(schema),
    )


class Function:
  name: str
  desc: str
  args: Dict[str, Arg]
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
    or_replace, self.name, args, ret_comment, ret_type = doc.obj_match

    if or_replace is not None:
      self._error(
          f'Function "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')

    # Ignore internal functions.
    if _is_internal(self.name):
      return None

    name = self._parse_name()

    if not _is_snake_case(name):
      self._error(f'Function name "{name}" is not snake_case'
                  f' (should be {name.casefold()})')

    ret_desc = None if ret_comment is None else parse_comment(ret_comment)
    if not ret_desc:
      self._error(f'Function "{name}": return description is missing')

    return Function(
        name=name,
        desc=self._parse_desc_not_empty(doc.description),
        args=self._parse_args(args),
        return_type=ret_type,
        return_desc=ret_desc,
    )


class TableFunction:
  name: str
  desc: str
  cols: Dict[str, Arg]
  args: Dict[str, Arg]

  def __init__(self, name, desc, cols, args):
    self.name = name
    self.desc = desc
    self.cols = cols
    self.args = args


class TableFunctionDocParser(AbstractDocParser):
  """Parses documentation for table function statements."""

  def __init__(self, path: str, module: str):
    super().__init__(path, module)

  def parse(self, doc: DocsExtractor.Extract) -> Optional[TableFunction]:
    or_replace, self.name, args, ret_comment, columns = doc.obj_match

    if or_replace is not None:
      self._error(
          f'Function "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')
      return

    # Ignore internal functions.
    if _is_internal(self.name):
      return None

    name = self._parse_name()

    if not _is_snake_case(name):
      self._error(f'Function name "{name}" is not snake_case'
                  f' (should be "{name.casefold()}")')

    return TableFunction(
        name=name,
        desc=self._parse_desc_not_empty(doc.description),
        cols=self._parse_columns(columns),
        args=self._parse_args(args),
    )


class Macro:
  name: str
  desc: str
  return_desc: str
  return_type: str
  args: Dict[str, Arg]

  def __init__(self, name: str, desc: str, return_desc: str, return_type: str,
               args: Dict[str, Arg]):
    self.name = name
    self.desc = desc
    self.return_desc = return_desc
    self.return_type = return_type
    self.args = args


class MacroDocParser(AbstractDocParser):
  """Parses documentation for macro statements."""

  def __init__(self, path: str, module: str):
    super().__init__(path, module)

  def parse(self, doc: DocsExtractor.Extract) -> Optional[Macro]:
    or_replace, self.name, args, return_desc, return_type = doc.obj_match

    if or_replace is not None:
      self._error(
          f'Function "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')

    # Ignore internal macros.
    if _is_internal(self.name):
      return None

    name = self._parse_name()

    if not _is_snake_case(name):
      self._error(f'Macro name "{name}" is not snake_case'
                  f' (should be "{name.casefold()}")')

    return Macro(
        name=name,
        desc=self._parse_desc_not_empty(doc.description),
        return_desc=parse_comment(return_desc),
        return_type=return_type,
        args=self._parse_args(args),
    )


class Include:
  package: str
  module: str
  module_as_list: List[str]

  def __init__(self, package: str, module: str, module_as_list: List[str]):
    self.package = package
    self.module = module
    self.module_as_list = module_as_list


class IncludeParser(AbstractDocParser):
  """Parses the includes of module."""

  def __init__(self, path: str, module: str):
    super().__init__(path, module)

  def parse(self, doc: DocsExtractor.Extract) -> Optional[Include]:
    self.module = list(doc.obj_match)[0]
    module_as_list = self.module.split('.')

    return Include(
        package=module_as_list[0],
        module=self.module,
        module_as_list=module_as_list,
    )


class ParsedModule:
  """Data class containing all of the documentation of single SQL file"""
  package_name: str = ""
  module_as_list: List[str]
  module: str
  errors: List[str] = []
  table_views: List[TableOrView] = []
  functions: List[Function] = []
  table_functions: List[TableFunction] = []
  macros: List[Macro] = []
  includes: List[Include]

  def __init__(self, package_name: str, module_as_list: List[str],
               errors: List[str], table_views: List[TableOrView],
               functions: List[Function], table_functions: List[TableFunction],
               macros: List[Macro], includes: List[Include]):
    self.package_name = package_name
    self.module_as_list = module_as_list
    self.module = ".".join(module_as_list)
    self.errors = errors
    self.table_views = table_views
    self.functions = functions
    self.table_functions = table_functions
    self.macros = macros
    self.includes = includes


def parse_file(path: str, sql: str) -> Optional[ParsedModule]:
  """Reads the provided SQL and, if possible, generates a dictionary with data
    from documentation together with errors from validation of the schema."""
  if sys.platform.startswith('win'):
    path = path.replace('\\', '/')

  module_as_list: List[str] = path.split('/stdlib/')[-1].split(".sql")[0].split(
      '/')

  # Get package name
  package_name = module_as_list[0]

  # Disable support for `deprecated` package
  if package_name == "deprecated":
    return

  # Extract all the docs from the SQL.
  extractor = DocsExtractor(path, package_name, sql)
  docs = extractor.extract()
  if extractor.errors:
    return ParsedModule(package_name, module_as_list, extractor.errors, [], [],
                        [], [], [])

  # Parse the extracted docs.
  errors: List[str] = []
  table_views: List[TableOrView] = []
  functions: List[Function] = []
  table_functions: List[TableFunction] = []
  macros: List[Macro] = []
  includes: List[Include] = []
  for doc in docs:
    if doc.obj_kind == ObjKind.table_view:
      parser = TableViewDocParser(path, package_name)
      res = parser.parse(doc)
      if res:
        table_views.append(res)
      errors += parser.errors
    if doc.obj_kind == ObjKind.function:
      parser = FunctionDocParser(path, package_name)
      res = parser.parse(doc)
      if res:
        functions.append(res)
      errors += parser.errors
    if doc.obj_kind == ObjKind.table_function:
      parser = TableFunctionDocParser(path, package_name)
      res = parser.parse(doc)
      if res:
        table_functions.append(res)
      errors += parser.errors
    if doc.obj_kind == ObjKind.macro:
      parser = MacroDocParser(path, package_name)
      res = parser.parse(doc)
      if res:
        macros.append(res)
      errors += parser.errors
    if doc.obj_kind == ObjKind.include:
      parser = IncludeParser(path, package_name)
      res = parser.parse(doc)
      if res:
        includes.append(res)
      errors += parser.errors

  return ParsedModule(package_name, module_as_list, errors, table_views,
                      functions, table_functions, macros, includes)
