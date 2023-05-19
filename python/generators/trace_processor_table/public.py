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


@dataclass(frozen=True)
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
  HIDDEN = auto()
  DENSE = auto()
  SET_ID = auto()


@dataclass(frozen=True)
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


@dataclass(frozen=True)
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


@dataclass(frozen=True)
class TableDoc:
  """
  Documentation for the C++ table.

  Used to generate reference pages on the docs website.

  Attributes:
    doc: Freeform docstring for the table.
    group: The group of tables this table belongs to. Examples include "Tracks",
    "Events", "ART Heap Graphs" etc: see the docs page for all the existing
    groups.
    columns: Documentation for each table column.
    skip_id_and_type: Skips publishing these columns in the documentation.
    Should only be used when these columns are not meaningful or are aliased to
    something better.
  """
  doc: str
  group: str
  columns: Dict[str, Union[ColumnDoc, str]]
  skip_id_and_type: bool = False


@dataclass(frozen=True)
class WrappingSqlView:
  """
  Specifies information about SQL view wrapping a table.

  Useful for tables which are not exposed directly to
  SQL but instead are wrapped with a SQL view.

  Attributes:
    view_name: The name of the SQL view exposed to SQL.
  """
  view_name: str


@dataclass(frozen=True)
class Table:
  """
  Representation of of a C++ table.

  Attributes:
    python_module: Path to the Python module this table is defined in. Always
    pass __file__.
    class_name: Name of the C++ table class.
    sql_name: Name of the table in SQL.
    columns: The columns in this table.
    tabledoc: Documentation for this table. Can include documentation overrides
    for auto-added columns (i.e. id and type) and aliases added in
    |wrapping_sql_view|.
    parent: The parent table for this table. All columns are inherited from the
    specified table.
    wrapping_sql_view: See |WrappingSqlView|.
  """
  python_module: str
  class_name: str
  sql_name: str
  columns: List[Column]
  parent: Optional['Table'] = None
  tabledoc: Optional[TableDoc] = None
  wrapping_sql_view: Optional[WrappingSqlView] = None


@dataclass(frozen=True)
class CppInt64(CppColumnType):
  """Represents the int64_t C++ type."""


@dataclass(frozen=True)
class CppUint32(CppColumnType):
  """Represents the uint32_t C++ type."""


@dataclass(frozen=True)
class CppInt32(CppColumnType):
  """Represents the int32_t C++ type."""


@dataclass(frozen=True)
class CppDouble(CppColumnType):
  """Represents the double C++ type."""


@dataclass(frozen=True)
class CppString(CppColumnType):
  """Represents the StringPool::Id C++ type."""


@dataclass(frozen=True)
class CppOptional(CppColumnType):
  """Represents the base::Optional C++ type."""
  inner: CppColumnType


@dataclass(frozen=True)
class CppTableId(CppColumnType):
  """Represents the Table::Id C++ type."""
  table: Table


@dataclass(frozen=True)
class CppSelfTableId(CppColumnType):
  """Represents the Id C++ type."""


@dataclass(frozen=True)
class Alias(CppColumnType):
  """Represents a column which aliases another column.

  Aliasing refers to re-exporting a column with a different name. This is useful
  especially for exporting "id" columns which names which associate it to the
  table name: e.g. exporting thread.id as thread.utid"""
  underlying_column: str
