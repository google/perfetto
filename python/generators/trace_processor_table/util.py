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
from typing import Optional

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


@dataclass()
class ParsedType:
  """Result of parsing a CppColumnType into its parts."""
  cpp_type: str
  is_optional: bool = False
  is_alias: bool = False
  alias_underlying_name: Optional[str] = None
  is_self_id: bool = False
  id_table: Optional[Table] = None


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
  new_cols_doc = {
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
