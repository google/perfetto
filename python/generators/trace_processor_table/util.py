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

import dataclasses
from dataclasses import dataclass
import importlib
import sys
from typing import Dict
from typing import List
from typing import Set
from typing import Optional
from typing import Union

from python.generators.trace_processor_table.public import Alias
from python.generators.trace_processor_table.public import Column
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import CppColumnType
from python.generators.trace_processor_table.public import CppDouble
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppSelfTableId
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table


@dataclass
class ParsedType:
  """Result of parsing a CppColumnType into its parts."""
  cpp_type: str
  is_optional: bool = False
  is_alias: bool = False
  alias_underlying_name: Optional[str] = None
  is_self_id: bool = False
  id_table: Optional[Table] = None

  def cpp_type_with_optionality(self) -> str:
    """Returns the C++ type wrapping with base::Optional if necessary."""

    # ThreadTable and ProcessTable are special for legacy reasons as they were
    # around even before the advent of C++ macro tables. Because of this a lot
    # of code was written assuming that upid and utid were uint32 (e.g. indexing
    # directly into vectors using them) and it was decided this behaviour was
    # too expensive in engineering cost to fix given the trivial benefit. For
    # this reason, continue to maintain this illusion.
    if self.id_table and self.id_table.class_name in ('ThreadTable',
                                                      'ProcessTable'):
      cpp_type = 'uint32_t'
    else:
      cpp_type = self.cpp_type
    if self.is_optional:
      return f'std::optional<{cpp_type}>'
    return cpp_type


@dataclass(frozen=True)
class ParsedColumn:
  """Representation of a column parsed from a Python definition."""

  column: Column
  doc: Optional[ColumnDoc]

  # Whether this column is the implicit "id" column which is added by while
  # parsing the tables rather than by the user.
  is_implicit_id: bool = False

  # Whether this column is the implicit "type" column which is added by while
  # parsing the tables rather than by the user.
  is_implicit_type: bool = False

  # Whether this column comes from copying a column from the ancestor. If this
  # is set to false, the user explicitly specified it for this table.
  is_ancestor: bool = False


@dataclass(frozen=True)
class ParsedTable:
  """Representation of a table parsed from a Python definition."""

  table: Table
  columns: List[ParsedColumn]


def parse_type_with_cols(table: Table, cols: List[Column],
                         col_type: CppColumnType) -> ParsedType:
  """Parses a CppColumnType into its constiuent parts."""

  if isinstance(col_type, CppInt64):
    return ParsedType('int64_t')
  if isinstance(col_type, CppInt32):
    return ParsedType('int32_t')
  if isinstance(col_type, CppUint32):
    return ParsedType('uint32_t')
  if isinstance(col_type, CppDouble):
    return ParsedType('double')
  if isinstance(col_type, CppString):
    return ParsedType('StringPool::Id')

  if isinstance(col_type, Alias):
    col = next(c for c in cols if c.name == col_type.underlying_column)
    return ParsedType(
        parse_type(table, col.type).cpp_type,
        is_alias=True,
        alias_underlying_name=col.name)

  if isinstance(col_type, CppTableId):
    return ParsedType(
        f'{col_type.table.class_name}::Id', id_table=col_type.table)

  if isinstance(col_type, CppSelfTableId):
    return ParsedType(
        f'{table.class_name}::Id', is_self_id=True, id_table=table)

  if isinstance(col_type, CppOptional):
    inner = parse_type(table, col_type.inner)
    assert not inner.is_optional, 'Nested optional not allowed'
    return dataclasses.replace(inner, is_optional=True)

  raise Exception(f'Unknown type {col_type}')


def parse_type(table: Table, col_type: CppColumnType) -> ParsedType:
  """Parses a CppColumnType into its constiuent parts."""
  return parse_type_with_cols(table, table.columns, col_type)


def typed_column_type(table: Table, col: ParsedColumn) -> str:
  """Returns the TypedColumn/IdColumn C++ type for a given column."""

  parsed = parse_type(table, col.column.type)
  if col.is_implicit_id:
    return f'IdColumn<{parsed.cpp_type}>'
  return f'TypedColumn<{parsed.cpp_type_with_optionality()}>'


def find_table_deps(table: Table) -> List[Table]:
  """Finds all the other table class names this table depends on.

  By "depends", we mean this table in C++ would need the dependency to be
  defined (or included) before this table is defined."""

  deps: Dict[str, Table] = {}
  if table.parent:
    deps[table.parent.class_name] = table.parent
  for c in table.columns:
    # Aliases cannot have dependencies so simply ignore them: trying to parse
    # them before adding implicit columns can cause issues.
    if isinstance(c.type, Alias):
      continue
    id_table = parse_type(table, c.type).id_table
    if id_table:
      deps[id_table.class_name] = id_table
  return list(deps.values())


def public_sql_name(table: Table) -> str:
  """Extracts SQL name for the table which should be publicised."""

  wrapping_view = table.wrapping_sql_view
  return wrapping_view.view_name if wrapping_view else table.sql_name


def _create_implicit_columns_for_root(table: Table) -> List[ParsedColumn]:
  """Given a root table, returns the implicit id and type columns."""
  assert table.parent is None

  sql_name = public_sql_name(table)
  id_doc = table.tabledoc.columns.get('id') if table.tabledoc else None
  type_doc = table.tabledoc.columns.get('type') if table.tabledoc else None
  return [
      ParsedColumn(
          Column('id', CppSelfTableId(), ColumnFlag.SORTED),
          _to_column_doc(id_doc) if id_doc else ColumnDoc(
              doc=f'Unique identifier for this {sql_name}.'),
          is_implicit_id=True),
      ParsedColumn(
          Column('type', CppString(), ColumnFlag.NONE),
          _to_column_doc(type_doc) if type_doc else ColumnDoc(doc='''
                The name of the "most-specific" child table containing this
                row.
              '''),
          is_implicit_type=True,
      )
  ]


def _topological_sort_table_and_deps(parsed: List[Table]) -> List[Table]:
  """Topologically sorts a list of tables (i.e. dependenices appear earlier).

  See [1] for information on a topological sort. We do this to allow
  dependencies to be processed and appear ealier than their dependents.

  [1] https://en.wikipedia.org/wiki/Topological_sorting"""
  visited: Set[str] = set()
  result: List[Table] = []

  # Topological sorting is really just a DFS where we put the nodes in the list
  # after any dependencies.
  def dfs(t: Table):
    if t.class_name in visited:
      return
    visited.add(t.class_name)

    for dep in find_table_deps(t):
      dfs(dep)
    result.append(t)

  for p in parsed:
    dfs(p)
  return result


def _to_column_doc(doc: Union[ColumnDoc, str, None]) -> Optional[ColumnDoc]:
  """Cooerces a user specified ColumnDoc or string into a ColumnDoc."""

  if doc is None or isinstance(doc, ColumnDoc):
    return doc
  return ColumnDoc(doc=doc)


def parse_tables_from_modules(modules: List[str]) -> List[ParsedTable]:
  """Creates a list of tables with the associated paths."""

  # Create a mapping from the table to a "parsed" version of the table.
  tables: Dict[str, Table] = {}
  for module in modules:
    imported = importlib.import_module(module)
    run_tables: List[Table] = imported.__dict__['ALL_TABLES']
    for table in run_tables:
      existing_table = tables.get(table.class_name)
      assert not existing_table or existing_table == table
      tables[table.class_name] = table

  # Sort all the tables: note that this list may include tables which are not
  # in |tables| dictionary due to dependencies on tables which live in a file
  # not covered by |input_paths|.
  sorted_tables = _topological_sort_table_and_deps(list(tables.values()))

  parsed_tables: Dict[str, ParsedTable] = {}
  for table in sorted_tables:
    parsed_columns: List[ParsedColumn]
    if table.parent:
      parsed_parent = parsed_tables[table.parent.class_name]
      parsed_columns = [
          dataclasses.replace(c, is_ancestor=True)
          for c in parsed_parent.columns
      ]
    else:
      parsed_columns = _create_implicit_columns_for_root(table)

    for c in table.columns:
      doc = table.tabledoc.columns.get(c.name) if table.tabledoc else None
      parsed_columns.append(ParsedColumn(c, _to_column_doc(doc)))
    parsed_tables[table.class_name] = ParsedTable(table, parsed_columns)

  # Only return tables which come directly from |input_paths|. This stops us
  # generating tables which were not requested.
  return [
      parsed_tables[p.class_name]
      for p in sorted_tables
      if p.class_name in tables
  ]
