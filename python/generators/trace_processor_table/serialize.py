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
from python.generators.trace_processor_table.public import ColumnFlag
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import ParsedColumn
from python.generators.trace_processor_table.util import to_cpp_flags


class ColumnSerializer:
  """Functions for serializing a single Column in a table into C++."""

  def __init__(self, table: ParsedTable, column: ParsedColumn, col_index: int):
    self.col_index = col_index
    self.parsed_col = column
    self.col = self.parsed_col.column
    self.name = self.col.name
    self.flags = self.col.flags
    self.typed_column_type = table.typed_column_type(self.parsed_col)
    self.cpp_type = table.parse_type(self.col.type).cpp_type_with_optionality()

    self.is_implicit_id = self.parsed_col.is_implicit_id
    self.is_implicit_type = self.parsed_col.is_implicit_type
    self.is_ancestor = self.parsed_col.is_ancestor

  def colindex(self) -> str:
    return f'    static constexpr uint32_t {self.name} = {self.col_index};'

  def coltype_enum(self) -> str:
    return f'    using {self.name} = {self.typed_column_type};'

  def row_field(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'    {self.cpp_type} {self.name};'

  def row_param(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    return f'{self.cpp_type} in_{self.name} = {{}}'

  def parent_row_initializer(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if not self.is_ancestor:
      return None
    return f'std::move(in_{self.name})'

  def row_initializer(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'{self.name}(std::move(in_{self.name}))'

  def const_row_ref_getter(self) -> Optional[str]:
    return f'''ColumnType::{self.name}::type {self.name}() const {{
      return table_->{self.name}()[row_number_];
    }}'''

  def row_ref_getter(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    return f'''void set_{self.name}(
        ColumnType::{self.name}::non_optional_type v) {{
      return mutable_table()->mutable_{self.name}()->Set(row_number_, v);
    }}'''

  def flag(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
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
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None

    storage = f'ColumnStorage<ColumnType::{self.name}::stored_type>'
    # TODO(lalitm): add support for dense columns.
    return f'''{self.name}_({storage}::Create<false>())'''

  def column_init(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'''
    columns_.emplace_back("{self.name}", &{self.name}_, ColumnFlag::{self.name},
                          this, static_cast<uint32_t>(columns_.size()),
                          overlay_idx);
    '''

  def shrink_to_fit(self) -> Optional[str]:
    if self.is_implicit_id:
      return None
    if self.is_ancestor:
      return None
    return f'    {self.name}_.ShrinkToFit();'

  def append(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
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
    if self.is_implicit_id or self.is_implicit_type:
      return None
    return f'''
  {self.typed_column_type}* mutable_{self.name}() {{
    return static_cast<ColumnType::{self.name}*>(
        &columns_[ColumnIndex::{self.name}]);
  }}
  '''

  def storage(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    name = self.name
    return f'  ColumnStorage<ColumnType::{name}::stored_type> {name}_;'


class TableSerializer(object):
  """Functions for seralizing a single Table into C++."""

  def __init__(self, parsed: ParsedTable):
    self.table = parsed.table
    self.table_name = parsed.table.class_name
    self.column_serializers = []

    if parsed.table.parent:
      self.parent_class_name = parsed.table.parent.class_name
    else:
      self.parent_class_name = 'macros_internal::RootParentTable'

    self.column_serializers = []
    for c in parsed.columns:
      # Aliases should be ignored as they are handled in SQL currently.
      if isinstance(c.column.type, Alias):
        continue
      self.column_serializers.append(
          ColumnSerializer(parsed, c, len(self.column_serializers)))

  def foreach_col(self, serialize_fn, delimiter='\n') -> str:
    lines = []
    for c in self.column_serializers:
      serialized = serialize_fn(c)
      if serialized:
        lines.append(serialized.lstrip('\n').rstrip())
    return delimiter.join(lines).strip()

  def id_defn(self) -> str:
    if self.table.parent:
      return f'''
  using Id = {self.table.parent.class_name}::Id;
    '''
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
    parent_row_init = self.foreach_col(
        ColumnSerializer.parent_row_initializer, delimiter=', ')
    row_init = self.foreach_col(
        ColumnSerializer.row_initializer, delimiter=',\n          ')
    return f'''
  struct Row : public {self.parent_class_name}::Row {{
    Row({param},
        std::nullptr_t = nullptr)
        : {self.parent_class_name}::Row({parent_row_init}),
          {row_init} {{
      type_ = "{self.table.sql_name}";
    }}
    {self.foreach_col(ColumnSerializer.row_field)}
  }};
    '''

  def const_row_reference_struct(self) -> str:
    row_ref_getters = self.foreach_col(
        ColumnSerializer.const_row_ref_getter, delimiter='\n    ')
    return f'''
  class ConstRowReference : public macros_internal::AbstractConstRowReference<
    {self.table_name}, RowNumber> {{
   public:
    ConstRowReference(const {self.table_name}* table, uint32_t row_number)
        : AbstractConstRowReference(table, row_number) {{}}

    {row_ref_getters}
  }};
  static_assert(std::is_trivially_destructible<ConstRowReference>::value,
                "Inheritance used without trivial destruction");
    '''

  def row_reference_struct(self) -> str:
    row_ref_getters = self.foreach_col(
        ColumnSerializer.row_ref_getter, delimiter='\n    ')
    return f'''
  class RowReference : public ConstRowReference {{
   public:
    RowReference(const {self.table_name}* table, uint32_t row_number)
        : ConstRowReference(table, row_number) {{}}

    {row_ref_getters}

   private:
    {self.table_name}* mutable_table() const {{
      return const_cast<{self.table_name}*>(table_);
    }}
  }};
  static_assert(std::is_trivially_destructible<RowReference>::value,
                "Inheritance used without trivial destruction");
    '''

  def constructor(self) -> str:
    col_init = self.foreach_col(
        ColumnSerializer.storage_init, delimiter=',\n        ')
    if self.table.parent:
      parent_param = f', {self.parent_class_name}* parent'
      parent_arg = 'parent'
      parent_init = 'parent_(parent), '
    else:
      parent_param = ''
      parent_arg = 'nullptr'
      parent_init = ''
    return f'''
  explicit {self.table_name}(StringPool* pool{parent_param})
      : macros_internal::MacroTable(pool, {parent_arg}),
        {parent_init}{col_init} {{
    uint32_t overlay_idx = static_cast<uint32_t>(overlays_.size()) - 1;
    {self.foreach_col(ColumnSerializer.column_init)}
  }}
    '''

  def parent_field(self) -> str:
    if self.table.parent:
      return f'''
  {self.parent_class_name}* parent_ = nullptr;
      '''
    return ''

  def insert_common(self) -> str:
    if self.table.parent:
      return '''
    Id id = Id{parent_->Insert(row).id};
    UpdateOverlaysAfterParentInsert();
      '''
    return '''
    Id id = Id{row_number};
    type_.Append(string_pool_->InternString(row.type()));
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
    Id id;
  }};
  struct ColumnFlag {{
    {self.foreach_col(ColumnSerializer.flag)}
  }};

  class RowNumber;
  class ConstRowReference;
  class RowReference;

  class RowNumber : public macros_internal::AbstractRowNumber<
      {self.table_name}, ConstRowReference, RowReference> {{
   public:
    explicit RowNumber(uint32_t row_number)
        : AbstractRowNumber(row_number) {{}}
  }};
  static_assert(std::is_trivially_destructible<RowNumber>::value,
                "Inheritance used without trivial destruction");

  {self.const_row_reference_struct().strip()}
  {self.row_reference_struct().strip()}

  {self.constructor().strip()}
  ~{self.table_name}() override;

  static const char* Name() {{ return "{self.table.sql_name}"; }}

  void ShrinkToFit() {{
    {self.foreach_col(ColumnSerializer.shrink_to_fit)}
  }}

  base::Optional<ConstRowReference> FindById(Id find_id) const {{
    base::Optional<uint32_t> row = id().IndexOf(find_id);
    return row ? base::make_optional(ConstRowReference(this, *row))
               : base::nullopt;
  }}

  base::Optional<RowReference> FindById(Id find_id) {{
    base::Optional<uint32_t> row = id().IndexOf(find_id);
    return row ? base::make_optional(RowReference(this, *row)) : base::nullopt;
  }}

  IdAndRow Insert(const Row& row) {{
    uint32_t row_number = row_count();
    {self.insert_common().strip()}
    {self.foreach_col(ColumnSerializer.append)}
    UpdateSelfOverlayAfterInsert();
    return IdAndRow{{row_number, std::move(id)}};
  }}

  {self.foreach_col(ColumnSerializer.accessor)}

  {self.foreach_col(ColumnSerializer.mutable_accessor)}

 private:
  {self.parent_field().strip()}
  {self.foreach_col(ColumnSerializer.storage)}
}};
  '''.strip('\n')


def serialize_header(ifdef_guard: str, tables: List[ParsedTable],
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
