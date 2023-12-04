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
from python.generators.trace_processor_table.util import parse_type
from python.generators.trace_processor_table.util import typed_column_type


class ColumnSerializer:
  """Functions for serializing a single Column in a table into C++."""

  def __init__(self, table: ParsedTable, column: ParsedColumn, col_index: int):
    self.col_index = col_index
    self.parsed_col = column
    self.col = self.parsed_col.column
    self.name = self.col.name
    self.flags = self.col.flags
    self.typed_column_type = typed_column_type(table.table, self.parsed_col)
    self.cpp_type = parse_type(table.table,
                               self.col.type).cpp_type_with_optionality()

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
    dense = str(ColumnFlag.DENSE in self.flags).lower()
    return f'''{self.name}_({storage}::Create<{dense}>())'''

  def column_init(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'''
    columns_.emplace_back("{self.name}", &{self.name}_, ColumnFlag::{self.name},
                          this, static_cast<uint32_t>(columns_.size()),
                          olay_idx);
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

  def iterator_getter(self) -> Optional[str]:
    name = self.name
    return f'''
    ColumnType::{self.name}::type {name}() const {{
      const auto& col = table_->{name}();
      return col.GetAtIdx(its_[col.overlay_index()].index());
    }}
    '''

  def iterator_setter(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    return f'''
      void set_{self.name}(ColumnType::{self.name}::non_optional_type v) {{
        auto* col = mutable_table_->mutable_{self.name}();
        col->SetAtIdx(its_[col->overlay_index()].index(), v);
      }}
    '''

  def static_schema(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    return f'''
    schema.columns.emplace_back(Table::Schema::Column{{
        "{self.name}", ColumnType::{self.name}::SqlValueType(), false,
        {str(ColumnFlag.SORTED in self.flags).lower()},
        {str(ColumnFlag.HIDDEN in self.flags).lower()},
        {str(ColumnFlag.SET_ID in self.flags).lower()}}});
    '''

  def row_eq(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    return f'ColumnType::{self.name}::Equals({self.name}, other.{self.name})'

  def extend_parent_param(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'ColumnStorage<ColumnType::{self.name}::stored_type> {self.name}'

  def extend_parent_param_arg(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'std::move({self.name})'

  def static_assert_flags(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'''
      static_assert(
        Column::IsFlagsAndTypeValid<ColumnType::{self.name}::stored_type>(
          ColumnFlag::{self.name}),
        "Column type and flag combination is not valid");
    '''

  def extend_nullable_vector(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'''
    PERFETTO_DCHECK({self.name}.size() == parent_overlay.size());
    {self.name}_ = std::move({self.name});
    '''


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
    parent_separator = ',' if row_init else ''
    row_eq = self.foreach_col(ColumnSerializer.row_eq, delimiter=' &&\n       ')
    return f'''
  struct Row : public {self.parent_class_name}::Row {{
    Row({param},
        std::nullptr_t = nullptr)
        : {self.parent_class_name}::Row({parent_row_init}){parent_separator}
          {row_init} {{
      type_ = "{self.table.sql_name}";
    }}
    {self.foreach_col(ColumnSerializer.row_field)}

    bool operator==(const {self.table_name}::Row& other) const {{
      return type() == other.type() && {row_eq};
    }}
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
    storage_init = self.foreach_col(
        ColumnSerializer.storage_init, delimiter=',\n        ')
    if self.table.parent:
      parent_param = f', {self.parent_class_name}* parent'
      parent_arg = 'parent'
      parent_init = 'parent_(parent)' + (', ' if storage_init else '')
    else:
      parent_param = ''
      parent_arg = 'nullptr'
      parent_init = ''
    col_init = self.foreach_col(ColumnSerializer.column_init)
    if col_init:
      olay = 'uint32_t olay_idx = static_cast<uint32_t>(overlays_.size()) - 1;'
    else:
      olay = ''
    return f'''
  explicit {self.table_name}(StringPool* pool{parent_param})
      : macros_internal::MacroTable(pool, {parent_arg}),
        {parent_init}{storage_init} {{
    {self.foreach_col(ColumnSerializer.static_assert_flags)}
    {olay}
    {col_init}
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

  def const_iterator(self) -> str:
    iterator_getters = self.foreach_col(
        ColumnSerializer.iterator_getter, delimiter='\n')
    return f'''
  class ConstIterator;
  class ConstIterator : public macros_internal::AbstractConstIterator<
    ConstIterator, {self.table_name}, RowNumber, ConstRowReference> {{
   public:
    {iterator_getters}

   protected:
    explicit ConstIterator(const {self.table_name}* table,
                           std::vector<ColumnStorageOverlay> overlays)
        : AbstractConstIterator(table, std::move(overlays)) {{}}

    uint32_t CurrentRowNumber() const {{
      return its_.back().index();
    }}

   private:
    friend class {self.table_name};
    friend class macros_internal::AbstractConstIterator<
      ConstIterator, {self.table_name}, RowNumber, ConstRowReference>;
  }};
      '''

  def iterator(self) -> str:
    iterator_setters = self.foreach_col(
        ColumnSerializer.iterator_setter, delimiter='\n')
    return f'''
  class Iterator : public ConstIterator {{
    public:
    {iterator_setters}

    RowReference row_reference() const {{
      return RowReference(mutable_table_, CurrentRowNumber());
    }}

    private:
    friend class {self.table_name};

    explicit Iterator({self.table_name}* table,
                      std::vector<ColumnStorageOverlay> overlays)
        : ConstIterator(table, std::move(overlays)),
          mutable_table_(table) {{}}

    {self.table_name}* mutable_table_ = nullptr;
  }};
      '''

  def extend(self) -> str:
    if not self.table.parent:
      return ''
    params = self.foreach_col(
        ColumnSerializer.extend_parent_param, delimiter='\n, ')
    args = self.foreach_col(
        ColumnSerializer.extend_parent_param_arg, delimiter=', ')
    delim = ',' if params else ''
    return f'''
  static std::unique_ptr<Table> ExtendParent(
      const {self.parent_class_name}& parent{delim}
      {params}) {{
    return std::unique_ptr<Table>(new {self.table_name}(
        parent.string_pool(), parent, RowMap(0, parent.row_count()){delim}
        {args}));
  }}

  static std::unique_ptr<Table> SelectAndExtendParent(
      const {self.parent_class_name}& parent,
      std::vector<{self.parent_class_name}::RowNumber> parent_overlay{delim}
      {params}) {{
    std::vector<uint32_t> prs_untyped(parent_overlay.size());
    for (uint32_t i = 0; i < parent_overlay.size(); ++i) {{
      prs_untyped[i] = parent_overlay[i].row_number();
    }}
    return std::unique_ptr<Table>(new {self.table_name}(
        parent.string_pool(), parent, RowMap(std::move(prs_untyped)){delim}
        {args}));
  }}
    '''

  def extend_constructor(self) -> str:
    if not self.table.parent:
      return ''
    params = self.foreach_col(
        ColumnSerializer.extend_parent_param, delimiter='\n, ')
    if params:
      olay = 'uint32_t olay_idx = static_cast<uint32_t>(overlays_.size()) - 1;'
    else:
      olay = ''
    return f'''
  {self.table_name}(StringPool* pool,
            const {self.parent_class_name}& parent,
            const RowMap& parent_overlay{',' if params else ''}
            {params})
      : macros_internal::MacroTable(pool, parent, parent_overlay) {{
    {self.foreach_col(ColumnSerializer.static_assert_flags)}
    {self.foreach_col(ColumnSerializer.extend_nullable_vector)}

    {olay}
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

  {self.const_iterator().strip()}
  {self.iterator().strip()}

  struct IdAndRow {{
    Id id;
    uint32_t row;
    RowReference row_reference;
    RowNumber row_number;
  }};

  {self.constructor().strip()}
  ~{self.table_name}() override;

  static const char* Name() {{ return "{self.table.sql_name}"; }}

  static Table::Schema ComputeStaticSchema() {{
    Table::Schema schema;
    schema.columns.emplace_back(Table::Schema::Column{{
        "id", SqlValue::Type::kLong, true, true, false, false}});
    schema.columns.emplace_back(Table::Schema::Column{{
        "type", SqlValue::Type::kString, false, false, false, false}});
    {self.foreach_col(ColumnSerializer.static_schema)}
    return schema;
  }}

  ConstIterator IterateRows() const {{
    return ConstIterator(this, CopyOverlays());
  }}

  Iterator IterateRows() {{ return Iterator(this, CopyOverlays()); }}

  ConstIterator FilterToIterator(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor opt = RowMap::OptimizeFor::kMemory) const {{
    return ConstIterator(this, FilterAndApplyToOverlays(cs, opt));
  }}

  Iterator FilterToIterator(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor opt = RowMap::OptimizeFor::kMemory) {{
    return Iterator(this, FilterAndApplyToOverlays(cs, opt));
  }}

  void ShrinkToFit() {{
    {self.foreach_col(ColumnSerializer.shrink_to_fit)}
  }}

  std::optional<ConstRowReference> FindById(Id find_id) const {{
    std::optional<uint32_t> row = id().IndexOf(find_id);
    return row ? std::make_optional(ConstRowReference(this, *row))
               : std::nullopt;
  }}

  std::optional<RowReference> FindById(Id find_id) {{
    std::optional<uint32_t> row = id().IndexOf(find_id);
    return row ? std::make_optional(RowReference(this, *row)) : std::nullopt;
  }}

  IdAndRow Insert(const Row& row) {{
    uint32_t row_number = row_count();
    {self.insert_common().strip()}
    {self.foreach_col(ColumnSerializer.append)}
    UpdateSelfOverlayAfterInsert();
    return IdAndRow{{std::move(id), row_number, RowReference(this, row_number),
                     RowNumber(row_number)}};
  }}

  {self.extend().strip()}

  {self.foreach_col(ColumnSerializer.accessor)}

  {self.foreach_col(ColumnSerializer.mutable_accessor)}

 private:
  {self.extend_constructor().strip()}
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

#include "src/trace_processor/tables/macros_internal.h"

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


def to_cpp_flags(raw_flag: ColumnFlag) -> str:
  """Converts a ColumnFlag to the C++ flags which it represents

  It is not valid to call this function with ColumnFlag.NONE as in this case
  defaults for that column should be implicitly used."""

  assert raw_flag != ColumnFlag.NONE
  flags = []
  if ColumnFlag.SORTED in raw_flag:
    flags.append('Column::Flag::kSorted')
  if ColumnFlag.HIDDEN in raw_flag:
    flags.append('Column::Flag::kHidden')
  if ColumnFlag.DENSE in raw_flag:
    flags.append('Column::Flag::kDense')
  if ColumnFlag.SET_ID in raw_flag:
    flags.append('Column::Flag::kSetId')
  return ' | '.join(flags)
