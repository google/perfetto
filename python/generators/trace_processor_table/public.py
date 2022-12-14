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
"""Contains the public API for generating C++ tables."""

from dataclasses import dataclass
from enum import auto
from enum import Flag as enum_Flag
from typing import Dict
from typing import List
from typing import Optional
from typing import Union


@dataclass
class CppColumnType:
  """
  The type of a column on a C++ table.

  See below for subclasses of this class which can be used.
  """


class ColumnFlag(enum_Flag):
  """
  Flags which should be associated to the C++ table column.

  For more information on each option here, see the Column::Flag
  enum. Keep this in sync with Column::Flag.
  """
  NONE = 0
  SORTED = auto()
  SET_ID = auto()


@dataclass
class Column:
  """
  Representation of a column of a C++ table.

  Attributes:
    name: The name of the column.
    type: The type of the column.
    flags: Flags for the column, ColumnFlag.NONE by default.
  """
  name: str
  type: CppColumnType
  flags: ColumnFlag = ColumnFlag.NONE

  # Private fields used by the generator. Do not set these manually.
  _is_auto_added_id: bool = False
  _is_auto_added_type: bool = False


@dataclass
class ColumnDoc:
  """
  Documentation for the C++ table column.

  Used to generate reference pages on the docs website.

  Attributes:
    doc: Freeform docstring for the column.
    joinable: Indicates this column is joinable with a column
    from another table. Should have the format "table.column".
  """
  doc: str
  joinable: Optional[str] = None


@dataclass
class TableDoc:
  """
  Documentation for the C++ table.

  Used to generate reference pages on the docs website.

  Attributes:
    doc: Freeform docstring for the table.
    columns: Documentation for each table column.
    real_table_name: The real name of the table in SQL. Should be
    specified if wrapping the table with a view.
    group: The group of tables this table should be assciated
    with.
  """
  doc: str
  columns: Dict[str, Union[ColumnDoc, str]]
  real_sql_name: Optional[str] = None
  group: Optional[str] = None


@dataclass
class Table:
  """
  Representation of of a C++ table.

  Attributes:
    class_name: Name of the C++ table class.
    sql_name: Name of the table in SQL.
    columns: The columns in this table.
    tabledoc: Documentation for this table.
  """
  class_name: str
  sql_name: str
  columns: List[Column]
  tabledoc: TableDoc


@dataclass
class CppInt64(CppColumnType):
  """Represents the int64_t C++ type."""


@dataclass
class CppUint32(CppColumnType):
  """Represents the uint32_t C++ type."""


@dataclass
class CppInt32(CppColumnType):
  """Represents the int32_t C++ type."""


@dataclass
class CppString(CppColumnType):
  """Represents the StringPool::Id C++ type."""


@dataclass
class CppOptional(CppColumnType):
  """Represents the base::Optional C++ type."""
  inner: CppColumnType


@dataclass
class CppTableId(CppColumnType):
  """Represents the Table::Id C++ type."""
  table: Table


@dataclass
class CppSelfTableId(CppColumnType):
  """Represents the Id C++ type."""
