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

from typing import List
from typing import Optional

from python.generators.trace_processor_table.public import Alias
from python.generators.trace_processor_table.public import Column
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.util import parse_type
from python.generators.trace_processor_table.util import typed_column_type
from python.generators.trace_processor_table.util import to_cpp_flags


class ColumnSerializer:
  """Functions for serializing a single Column in a table into C++."""

  def __init__(self, table: Table, col_index: int):
    self.col_index = col_index
    self.col = table.columns[col_index]
    self.name = self.col.name
    self.flags = self.col.flags
    self.typed_column_type = typed_column_type(table, self.col)
    self.cpp_type = parse_type(table, self.col.type).cpp_type_with_optionality()

  def colindex(self) -> str:
    return f'    static constexpr uint32_t {self.name} = {self.col_index};'

  def coltype_enum(self) -> str:
    return f'    using {self.name} = {self.typed_column_type};'

  def row_field(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    return f'    {self.cpp_type} {self.name};'

  def row_param(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    return f'{self.cpp_type} in_{self.name} = {{}}'

  def row_initializer(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    return f'{self.name}(std::move(in_{self.name}))'

  def flag(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    default = f'ColumnType::{self.name}::default_flags()'
    if self.flags == ColumnFlag.NONE:
      flags = default
    else:
      flags = f'static_cast<uint32_t>({to_cpp_flags(self.flags)}) | {default}'
    return f'''
      static constexpr uint32_t {self.name} = {flags};
    '''

  def storage_init(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None

    storage = f'ColumnStorage<ColumnType::{self.name}::stored_type>'
    # TODO(lalitm): add support for dense columns.
    return f'''{self.name}_({storage}::Create<false>())'''

  def column_init(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    return f'''
    columns_.emplace_back("{self.name}", &{self.name}_, ColumnFlag::{self.name},
                          this, static_cast<uint32_t>(columns_.size()),
                          overlay_count);
    '''

  def shrink_to_fit(self) -> Optional[str]:
    if self.col._is_auto_added_id:
      return None
    return f'    {self.name}_.ShrinkToFit();'

  def append(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    return f'    mutable_{self.name}()->Append(std::move(row.{self.name}));'

  def accessor(self) -> Optional[str]:
    inner = f'columns_[ColumnIndex::{self.name}]'
    return f'''
  const {self.typed_column_type}& {self.name}() const {{
    return static_cast<const ColumnType::{self.name}&>({inner});
  }}
  '''

  def mutable_accessor(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    return f'''
  {self.typed_column_type}* mutable_{self.name}() {{
    return static_cast<ColumnType::{self.name}*>(
        &columns_[ColumnIndex::{self.name}]);
  }}
  '''

  def storage(self) -> Optional[str]:
    if self.col._is_auto_added_id or self.col._is_auto_added_type:
      return None
    name = self.name
    return f'  ColumnStorage<ColumnType::{name}::stored_type> {name}_;'


class TableSerializer(object):
  """Functions for seralizing a single Table into C++."""

  def __init__(self, table: Table):
    self.table = table
    self.table_name = table.class_name
    self.column_serializers = [
        ColumnSerializer(table, i) for i in range(len(table.columns))
    ]

  def foreach_col(self, serialize_fn, delimiter='\n') -> str:
    lines = []
    for c in self.column_serializers:
      serialized = serialize_fn(c)
      if serialized:
        lines.append(serialized.lstrip('\n').rstrip())
    return delimiter.join(lines).strip()

  def id_defn(self) -> str:
    return '''
  struct Id : public BaseId {
    Id() = default;
    explicit constexpr Id(uint32_t v) : BaseId(v) {}
  };
  static_assert(std::is_trivially_destructible<Id>::value,
                "Inheritance used without trivial destruction");
    '''

  def row_struct(self) -> str:
    param = self.foreach_col(
        ColumnSerializer.row_param, delimiter=',\n        ')
    row_init = self.foreach_col(
        ColumnSerializer.row_initializer, delimiter=',\n          ')
    return f'''
  struct Row : public macros_internal::RootParentTable::Row {{
    Row({param})
        : macros_internal::RootParentTable::Row(nullptr),
          {row_init} {{
      type_ = "{self.table.sql_name}";
    }}
    {self.foreach_col(ColumnSerializer.row_field)}
  }};
    '''

  def constructor(self) -> str:
    col_init = self.foreach_col(
        ColumnSerializer.storage_init, delimiter=',\n        ')
    return f'''
  explicit {self.table_name}(StringPool* pool)
      : macros_internal::MacroTable(pool, nullptr),
        {col_init} {{
    uint32_t overlay_count = static_cast<uint32_t>(overlays_.size()) - 1;
    {self.foreach_col(ColumnSerializer.column_init)}
  }}
    '''

  def serialize(self) -> str:
    return f'''
class {self.table_name} : public macros_internal::MacroTable {{
 public:
  {self.id_defn().lstrip()}
  struct ColumnIndex {{
    {self.foreach_col(ColumnSerializer.colindex)}
  }};
  struct ColumnType {{
    {self.foreach_col(ColumnSerializer.coltype_enum)}
  }};
  {self.row_struct().strip()}
  struct IdAndRow {{
    uint32_t row;
  }};
  struct ColumnFlag {{
    {self.foreach_col(ColumnSerializer.flag)}
  }};

  {self.constructor().strip()}
  ~{self.table_name}() override;

  static const char* Name() {{ return "{self.table.sql_name}"; }}

  void ShrinkToFit() {{
    {self.foreach_col(ColumnSerializer.shrink_to_fit)}
  }}

  IdAndRow Insert(const Row& row) {{
    uint32_t row_number = row_count();
    type_.Append(string_pool_->InternString(row.type()));
    {self.foreach_col(ColumnSerializer.append)}
    UpdateSelfOverlayAfterInsert();
    return IdAndRow{{row_number}};
  }}

  {self.foreach_col(ColumnSerializer.accessor)}

  {self.foreach_col(ColumnSerializer.mutable_accessor)}

 private:
  {self.foreach_col(ColumnSerializer.storage)}
}};
  '''.strip('\n')


def serialize_header(ifdef_guard: str, tables: List[Table],
                     include_paths: List[str]) -> str:
  """Serializes a table header file containing the given set of tables."""
  include_paths_str = '\n'.join([f'#include "{i}"' for i in include_paths])
  tables_str = '\n\n'.join([TableSerializer(t).serialize() for t in tables])
  return f'''
#ifndef {ifdef_guard}
#define {ifdef_guard}

#include "src/trace_processor/tables/macros.h"

{include_paths_str}

namespace perfetto {{
namespace trace_processor {{
namespace tables {{

{tables_str.strip()}

}}  // namespace tables
}}  // namespace trace_processor
}}  // namespace perfetto

#endif  // {ifdef_guard}
  '''.strip()
