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
    if self.id_table and (self.id_table.class_name == 'ThreadTable' or
                          self.id_table.class_name == 'ProcessTable'):
      cpp_type = 'uint32_t'
    else:
      cpp_type = self.cpp_type
    if self.is_optional:
      return f'base::Optional<{cpp_type}>'
    return cpp_type


def public_sql_name_for_table(table: Table) -> str:
  """Extracts SQL name for the table which should be publicised."""

  wrapping_view = table.wrapping_sql_view
  return wrapping_view.view_name if wrapping_view else table.sql_name


def parse_type(table: Table, col_type: CppColumnType) -> ParsedType:
  """Parses a CppColumnType into its constiuient parts."""

  if isinstance(col_type, CppInt64):
    return ParsedType('int64_t')
  if isinstance(col_type, CppInt32):
    return ParsedType('int32_t')
  if isinstance(col_type, CppUint32):
    return ParsedType('uint32_t')
  if isinstance(col_type, CppString):
    return ParsedType('StringPool::Id')

  if isinstance(col_type, Alias):
    col = next(c for c in table.columns if c.name == col_type.underlying_column)
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


def augment_table_with_auto_cols(table: Table) -> Table:
  """Adds auto-added columns (i.e. id and type) to the user defined table."""

  auto_cols = [
      Column('id', CppSelfTableId(), ColumnFlag.SORTED, _is_auto_added_id=True),
      Column('type', CppString(), ColumnFlag.NONE, _is_auto_added_type=True),
  ]
  public_sql_name = public_sql_name_for_table(table)
  new_cols_doc: Dict[str, Union[ColumnDoc, str]] = {
      'id':
          ColumnDoc(doc=f'Unique idenitifier for this {public_sql_name}.'),
      'type':
          ColumnDoc(doc='''
                The name of the "most-specific" child table containing this row.
              '''),
  }
  new_cols_doc.update(table.tabledoc.columns)
  return dataclasses.replace(
      table,
      columns=auto_cols + table.columns,
      tabledoc=dataclasses.replace(table.tabledoc, columns=new_cols_doc))


def find_table_deps(table: Table) -> Set[str]:
  """Finds all the other table class names this table depends on.

  By "depends", we mean this table in C++ would need the dependency to be
  defined (or included) before this table is defined."""
  deps: Set[str] = set()
  for c in table.columns:
    id_table = parse_type(table, c.type).id_table
    if id_table:
      deps.add(id_table.class_name)
  return deps


def topological_sort_tables(tables: List[Table]) -> List[Table]:
  """Topologically sorts a list of tables (i.e. dependenices appear earlier).

  See [1] for information on a topological sort. We do this to allow
  dependencies to be processed and appear ealier than their dependents.

  [1] https://en.wikipedia.org/wiki/Topological_sorting"""
  tables_by_name: dict[str, Table] = dict((t.class_name, t) for t in tables)
  visited: Set[str] = set()
  result: List[Table] = []

  # Topological sorting is really just a DFS where we put the nodes in the list
  # after any dependencies.
  def dfs(table_class_name: str):
    table = tables_by_name.get(table_class_name)
    # If the table is not found, that might be because it's not in this list of
    # tables. Just ignore this as its up to the caller to make sure any external
    # deps are handled correctly.
    if not table or table.class_name in visited:
      return
    visited.add(table.class_name)

    for dep in find_table_deps(table):
      dfs(dep)
    result.append(table)

  for table in tables:
    dfs(table.class_name)
  return result


def to_cpp_flags(raw_flag: ColumnFlag) -> str:
  """Converts a ColumnFlag to the C++ flags which it represents

  It is not valid to call this function with ColumnFlag.NONE as in this case
  defaults for that column should be implicitly used."""

  assert raw_flag != ColumnFlag.NONE
  flags = []
  if ColumnFlag.SORTED in raw_flag:
    flags.append('Column::Flag::kSorted')
  if ColumnFlag.SET_ID in raw_flag:
    flags.append('Column::Flag::kSetId')
  return ' | '.join(flags)


def typed_column_type(table: Table, col: Column) -> str:
  """Returns the TypedColumn/IdColumn C++ type for a given column."""

  parsed = parse_type(table, col.type)
  if col._is_auto_added_id:
    return f'IdColumn<{parsed.cpp_type}>'
  return f'TypedColumn<{parsed.cpp_type_with_optionality()}>'
