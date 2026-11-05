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

from dataclasses import dataclass
import re
import sys
from typing import Dict, List, Optional, NamedTuple

from python.generators.sql_processing.utils import ObjKind
from python.generators.sql_processing.utils import COLUMN_TYPES, MACRO_ARG_TYPES
from python.generators.sql_processing.utils import ALLOWED_PREFIXES
from python.generators.sql_processing.utils import OBJECT_NAME_ALLOWLIST
from python.generators.sql_processing.utils import CREATE_TABLE_ALLOWLIST
from python.generators.sql_processing.utils import is_internal


@dataclass
class DocParseOptions:
  # TODO(lalitm): set this to true and/or remove this check once Google3 modules
  # adhere to this check.
  enforce_every_column_set_is_documented: bool = False
  # Include internal artifacts (those starting with _) in the output
  include_internal: bool = False


def _is_snake_case(s: str) -> bool:
  return re.fullmatch(r'^[a-z_0-9]*$', s) is not None


def get_module_prefix_error(name: str, path: str, module: str) -> Optional[str]:
  """Returns error message if the name is not correct, None otherwise."""
  # Internal artifacts (starting with _) don't need to follow naming conventions
  if is_internal(name):
    return None
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
  type: str
  long_type: str
  description: str
  is_variadic: bool = False


def _make_arg(long_type: str, description: str, is_variadic: bool) -> Arg:
  if re.match(r'JOINID\(([_A-Za-z\.]*)\)', long_type):
    return Arg('JOINID', long_type, description, is_variadic)
  if re.match(r'ID\(([_A-Za-z\.]*)\)', long_type):
    return Arg('ID', long_type, description, is_variadic)
  return Arg(long_type, long_type, description, is_variadic)


class AbstractDocParser:

  def __init__(
      self,
      path: str,
      module: str,
      options: DocParseOptions,
  ):
    self.path = path
    self.module = module
    self.name = None
    self.errors = []
    self.options = options

  def _parse_name(self, upper: bool = False):
    assert self.name
    assert isinstance(self.name, str)
    module_prefix_error = get_module_prefix_error(self.name, self.path,
                                                  self.module)
    if module_prefix_error is not None:
      self._error(module_prefix_error)
    return self.name.strip()

  def _parse_desc_not_empty(self, desc: str):
    # Internal artifacts don't need descriptions
    if not desc and not is_internal(self.name):
      self._error('Description of the table/view/function/macro is missing')
    return desc.strip() if desc else ""

  def _validate_columns(
      self,
      columns: Dict[str, Arg],
      kind: ObjKind,
  ) -> Dict[str, Arg]:
    # Internal artifacts don't need column documentation
    is_internal_artifact = is_internal(self.name)

    if not columns and self.options.enforce_every_column_set_is_documented and not is_internal_artifact:
      self._error(
          'Description of the columns of table/view/function is missing')
      return columns

    for column_name, properties in columns.items():
      if not properties.description and not is_internal_artifact:
        self._error(
            f'Column "{column_name}" is missing a description. Please add a '
            'comment in front of the column definition')
        continue

      upper_arg_type = properties.type.upper()
      if kind is ObjKind.table_function:
        if upper_arg_type not in COLUMN_TYPES:
          self._error(
              f'Table function column "{column_name}" has unsupported type "{properties.type}".'
          )
      elif kind is ObjKind.table_view:
        if upper_arg_type not in COLUMN_TYPES:
          self._error(
              f'Table/view column "{column_name}" has unsupported type "{properties.type}".'
          )
      else:
        self._error(f'This Perfetto SQL object doesnt support columns".')

    return columns

  def _validate_args(self, args: Dict[str, Arg],
                     kind: ObjKind) -> Dict[str, Arg]:
    # Internal artifacts don't need arg documentation
    is_internal_artifact = is_internal(self.name)

    for arg in args:
      if not args[arg].description and not is_internal_artifact:
        self._error(f'Arg "{arg}" is missing a description. '
                    'Please add a comment in front of the arg definition.')

      upper_arg_type = args[arg].type.upper()
      if (kind is ObjKind.function or kind is ObjKind.table_function):
        if upper_arg_type not in COLUMN_TYPES:
          self._error(
              f'Function arg "{arg}" has unsupported type "{args[arg].type}".')
      elif (kind is ObjKind.macro):
        if upper_arg_type not in MACRO_ARG_TYPES:
          self._error(
              f'Macro arg "{arg}" has unsupported type "{args[arg].type}".')
      else:
        self._error(f'This Perfetto SQL object doesnt support types".')

    return args

  def _args_from_pfsql(self, entries: List[dict]) -> Dict[str, Arg]:
    result = {}
    for i, entry in enumerate(entries):
      if entry['is_variadic'] and i != len(entries) - 1:
        self._error('Variadic argument must be the last argument')
        return result
      result[entry['name']] = _make_arg(entry['type'], entry['description'],
                                        entry['is_variadic'])
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

  def __init__(self, path: str, module: str, options: DocParseOptions):
    super().__init__(path, module, options)

  def parse(self, or_replace: bool, perfetto_or_virtual: Optional[str],
            type: str, columns: Dict[str,
                                     Arg], desc: str) -> Optional[TableOrView]:
    # Skip internal artifacts early if not including them
    if is_internal(self.name) and not self.options.include_internal:
      return None

    if or_replace:
      self._error(
          f'{type} "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')
      return

    if (type.lower() == "table" and not perfetto_or_virtual and
        self.name not in CREATE_TABLE_ALLOWLIST):
      self._error(
          f'{type} "{self.name}": Can only expose CREATE PERFETTO tables')
      return

    cols = self._validate_columns(columns, ObjKind.table_view)
    return TableOrView(
        name=self._parse_name(),
        type=type,
        desc=self._parse_desc_not_empty(desc),
        cols=cols)


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

  def __init__(self, path: str, module: str, options: DocParseOptions):
    super().__init__(path, module, options)

  def parse(self, or_replace: bool, args: Dict[str, Arg], ret_type: str,
            ret_desc: Optional[str], desc: str) -> Optional[Function]:
    if or_replace:
      self._error(
          f'Function "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')

    # Ignore internal functions unless explicitly requested.
    if is_internal(self.name) and not self.options.include_internal:
      return None

    name = self._parse_name()

    if not _is_snake_case(name):
      self._error(f'Function name "{name}" is not snake_case'
                  f' (should be {name.casefold()})')

    # Internal artifacts don't need return descriptions
    if not ret_desc and not is_internal(name):
      self._error(f'Function "{name}": return description is missing')

    return Function(
        name=name,
        desc=self._parse_desc_not_empty(desc),
        args=self._validate_args(args, ObjKind.function),
        return_type=ret_type,
        return_desc=ret_desc if ret_desc else "",
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

  def __init__(self, path: str, module: str, options: DocParseOptions):
    super().__init__(path, module, options)

  def parse(self, or_replace: bool, args: Dict[str, Arg],
            columns: Dict[str, Arg], desc: str) -> Optional[TableFunction]:
    if or_replace:
      self._error(
          f'Function "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')
      return

    # Ignore internal functions unless explicitly requested.
    if is_internal(self.name) and not self.options.include_internal:
      return None

    name = self._parse_name()

    if not _is_snake_case(name):
      self._error(f'Function name "{name}" is not snake_case'
                  f' (should be "{name.casefold()}")')

    return TableFunction(
        name=name,
        desc=self._parse_desc_not_empty(desc),
        cols=self._validate_columns(columns, ObjKind.table_function),
        args=self._validate_args(args, ObjKind.table_function),
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

  def __init__(self, path: str, module: str, options: DocParseOptions):
    super().__init__(path, module, options)

  def parse(self, or_replace: bool, args: Dict[str, Arg], return_type: str,
            return_desc: str, desc: str) -> Optional[Macro]:
    if or_replace:
      self._error(
          f'Function "{self.name}": CREATE OR REPLACE is not allowed in stdlib '
          f'as standard library modules can only included once. Please just '
          f'use CREATE instead.')

    # Ignore internal macros unless explicitly requested.
    if is_internal(self.name) and not self.options.include_internal:
      return None

    name = self._parse_name()

    if not _is_snake_case(name):
      self._error(f'Macro name "{name}" is not snake_case'
                  f' (should be "{name.casefold()}")')

    return Macro(
        name=name,
        desc=self._parse_desc_not_empty(desc),
        return_desc=return_desc,
        return_type=return_type,
        args=self._validate_args(args, ObjKind.macro),
    )


class Include:
  package: str
  module: str
  module_as_list: List[str]

  def __init__(self, package: str, module: str, module_as_list: List[str]):
    self.package = package
    self.module = module
    self.module_as_list = module_as_list


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


def parse_module(
    path: str,
    module: dict,
    options: DocParseOptions = DocParseOptions(),
) -> Optional[ParsedModule]:
  """Validates a module from `pfsql parse`, which already did the parsing."""
  if sys.platform.startswith('win'):
    path = path.replace('\\', '/')

  rel_no_ext = re.sub(r'\.(?:pf\.)?sql$', '', path.split('/stdlib/')[-1])
  module_as_list: List[str] = rel_no_ext.split('/')
  package_name = module_as_list[0]
  if package_name == "deprecated":
    return None

  docs = module['docs']
  table_view_docs = {t['name']: t for t in docs['table_views']}
  errors: List[str] = list(module['errors'])
  table_views: List[TableOrView] = []
  functions: List[Function] = []
  table_functions: List[TableFunction] = []
  macros: List[Macro] = []

  for symbol in module['symbols']:
    if symbol['kind'] not in ('table', 'view'):
      continue
    parser = TableViewDocParser(path, package_name, options)
    parser.name = symbol['name']
    doc = table_view_docs.get(symbol['name'])
    if doc:
      res = parser.parse(doc['or_replace'], 'PERFETTO', doc['type'],
                         parser._args_from_pfsql(doc['columns']),
                         doc['description'])
    else:
      res = parser.parse(False,
                         None if symbol['form'] == 'plain' else 'VIRTUAL',
                         symbol['kind'].upper(), {}, '')
    if res:
      table_views.append(res)
    errors += parser.errors

  for doc in docs['functions']:
    if doc['is_table_function']:
      parser = TableFunctionDocParser(path, package_name, options)
      parser.name = doc['name']
      res = parser.parse(doc['or_replace'],
                         parser._args_from_pfsql(doc['args']),
                         parser._args_from_pfsql(doc['columns']),
                         doc['description'])
      if res:
        table_functions.append(res)
    else:
      parser = FunctionDocParser(path, package_name, options)
      parser.name = doc['name']
      res = parser.parse(doc['or_replace'],
                         parser._args_from_pfsql(doc['args']),
                         doc['return_type'], doc['return_description'],
                         doc['description'])
      if res:
        functions.append(res)
    errors += parser.errors

  for doc in docs['macros']:
    parser = MacroDocParser(path, package_name, options)
    parser.name = doc['name']
    res = parser.parse(doc['or_replace'], parser._args_from_pfsql(doc['args']),
                       doc['return_type'], doc['return_description'],
                       doc['description'])
    if res:
      macros.append(res)
    errors += parser.errors

  includes = [
      Include(
          package=name.split('.')[0],
          module=name,
          module_as_list=name.split('.')) for name in module['includes']
  ]
  return ParsedModule(package_name, module_as_list, errors, table_views,
                      functions, table_functions, macros, includes)
