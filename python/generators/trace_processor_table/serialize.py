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
from python.generators.trace_processor_table.util import data_layer_type
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

    parsed_type = parse_type(table.table, self.col.type)

    self.typed_column_type = typed_column_type(table.table, self.parsed_col)
    self.cpp_type = parsed_type.cpp_type_with_optionality()
    self.data_layer_type = data_layer_type(table.table, self.parsed_col)

    self.is_implicit_id = self.parsed_col.is_implicit_id
    self.is_implicit_type = self.parsed_col.is_implicit_type
    self.is_ancestor = self.parsed_col.is_ancestor
    self.is_string = parsed_type.cpp_type == 'StringPool::Id'
    self.is_optional = parsed_type.is_optional

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
    return f'in_{self.name}'

  def row_initializer(self) -> Optional[str]:
    if self.is_implicit_id or self.is_implicit_type:
      return None
    if self.is_ancestor:
      return None
    return f'{self.name}(in_{self.name})'

  def const_row_ref_getter(self) -> Optional[str]:
    return f'''ColumnType::{self.name}::type {self.name}() const {{
      return table()->{self.name}()[row_number_];
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
    AddColumnToVector(columns, "{self.name}", &self->{self.name}_, ColumnFlag::{self.name},
                      static_cast<uint32_t>(columns.size()), olay_idx);
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
    return f'    mutable_{self.name}()->Append(row.{self.name});'

  def accessor(self) -> Optional[str]:
    inner = f'columns()[ColumnIndex::{self.name}]'
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
        GetColumn(ColumnIndex::{self.name}));
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
      const auto& col = table()->{name}();
      return col.GetAtIdx(
        iterator_.StorageIndexForColumn(col.index_in_table()));
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
        ColumnLegacy::IsFlagsAndTypeValid<ColumnType::{self.name}::stored_type>(
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

  def storage_layer(self) -> Optional[str]:
    if self.is_ancestor:
      return None
    return f'''
  RefPtr<column::StorageLayer> {self.name}_storage_layer_;
  '''

  def null_layer(self) -> Optional[str]:
    if self.is_ancestor:
      return None
    if not self.is_optional or self.is_string:
      return f''
    return f'''
  RefPtr<column::OverlayLayer> {self.name}_null_layer_;
  '''

  def storage_layer_create(self) -> str:
    if self.is_ancestor:
      return f'''const_parent_->storage_layers()[ColumnIndex::{self.name}]'''
    return f'''{self.name}_storage_layer_'''

  def null_layer_create(self) -> str:
    if not self.is_optional or self.is_string:
      return f'{{}}'
    if self.is_ancestor:
      return f'''const_parent_->null_layers()[ColumnIndex::{self.name}]'''
    return f'''{self.name}_null_layer_'''

  def storage_layer_init(self) -> str:
    if self.is_ancestor:
      return f''
    if self.is_implicit_id:
      return f'{self.name}_storage_layer_(new column::IdStorage())'
    if self.is_string:
      return f'''{self.name}_storage_layer_(
          new column::StringStorage(string_pool(), &{self.name}_.vector()))'''
    if ColumnFlag.SET_ID in self.flags:
      return f'''{self.name}_storage_layer_(
          new column::SetIdStorage(&{self.name}_.vector()))'''
    if self.is_optional:
      return f'''{self.name}_storage_layer_(
          new column::NumericStorage<ColumnType::{self.name}::non_optional_stored_type>(
            &{self.name}_.non_null_vector(),
            ColumnTypeHelper<ColumnType::{self.name}::stored_type>::ToColumnType(),
            {str(ColumnFlag.SORTED in self.flags).lower()}))'''
    return f'''{self.name}_storage_layer_(
        new column::NumericStorage<ColumnType::{self.name}::non_optional_stored_type>(
          &{self.name}_.vector(),
          ColumnTypeHelper<ColumnType::{self.name}::stored_type>::ToColumnType(),
          {str(ColumnFlag.SORTED in self.flags).lower()}))'''

  def null_layer_init(self) -> str:
    if self.is_ancestor:
      return f''
    if not self.is_optional or self.is_string:
      return f''
    if ColumnFlag.DENSE in self.flags:
      return f'''{self.name}_null_layer_(new column::DenseNullOverlay({self.name}_.bv()))'''
    return f'''{self.name}_null_layer_(new column::NullOverlay({self.name}_.bv()))'''


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
  static_assert(std::is_trivially_destructible_v<Id>,
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
  static_assert(std::is_trivially_destructible_v<ConstRowReference>,
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
      return const_cast<{self.table_name}*>(table());
    }}
  }};
  static_assert(std::is_trivially_destructible_v<RowReference>,
                "Inheritance used without trivial destruction");
    '''

  def constructor(self) -> str:
    storage_init = self.foreach_col(
        ColumnSerializer.storage_init, delimiter=',\n        ')
    storage_layer_init = self.foreach_col(
        ColumnSerializer.storage_layer_init, delimiter=',\n        ')
    storage_layer_sep = '\n,' if storage_layer_init else ''
    null_layer_init = self.foreach_col(
        ColumnSerializer.null_layer_init, delimiter=',\n        ')
    null_layer_sep = '\n,' if null_layer_init else ''
    if self.table.parent:
      parent_param = f', {self.parent_class_name}* parent'
      parent_arg = 'parent'
      parent_init = 'parent_(parent), const_parent_(parent)' + (
          ', ' if storage_init else '')
    else:
      parent_param = ''
      parent_arg = 'nullptr'
      parent_init = ''
    col_init = self.foreach_col(ColumnSerializer.column_init)
    if col_init:
      olay = 'uint32_t olay_idx = OverlayCount(parent);'
    else:
      olay = ''
    storage_layer_create = self.foreach_col(
        ColumnSerializer.storage_layer_create, delimiter=',')
    null_layer_create = self.foreach_col(
        ColumnSerializer.null_layer_create, delimiter=',')
    return f'''
  static std::vector<ColumnLegacy> GetColumns(
      {self.table_name}* self,
      const macros_internal::MacroTable* parent) {{
    std::vector<ColumnLegacy> columns =
        CopyColumnsFromParentOrAddRootColumns(self, parent);
    {olay}
    {col_init}
    return columns;
  }}

  PERFETTO_NO_INLINE explicit {self.table_name}(StringPool* pool{parent_param})
      : macros_internal::MacroTable(
          pool,
          GetColumns(this, {parent_arg}),
          {parent_arg}),
        {parent_init}{storage_init}{storage_layer_sep}
        {storage_layer_init}{null_layer_sep}
        {null_layer_init} {{
    {self.foreach_col(ColumnSerializer.static_assert_flags)}
    OnConstructionCompletedRegularConstructor(
      {{{storage_layer_create}}},
      {{{null_layer_create}}});
  }}
    '''

  def parent_field(self) -> str:
    if self.table.parent:
      return f'''
  {self.parent_class_name}* parent_ = nullptr;
  const {self.parent_class_name}* const_parent_ = nullptr;
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
    type_.Append(string_pool()->InternString(row.type()));
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
                           Table::Iterator iterator)
        : AbstractConstIterator(table, std::move(iterator)) {{}}

    uint32_t CurrentRowNumber() const {{
      return iterator_.StorageIndexForLastOverlay();
    }}

   private:
    friend class {self.table_name};
    friend class macros_internal::AbstractConstIterator<
      ConstIterator, {self.table_name}, RowNumber, ConstRowReference>;
  }};
      '''

  def iterator(self) -> str:
    return f'''
  class Iterator : public ConstIterator {{
    public:
     RowReference row_reference() const {{
       return {{const_cast<{self.table_name}*>(table()), CurrentRowNumber()}};
     }}

    private:
     friend class {self.table_name};

     explicit Iterator({self.table_name}* table, Table::Iterator iterator)
        : ConstIterator(table, std::move(iterator)) {{}}
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
  static std::unique_ptr<{self.table_name}> ExtendParent(
      const {self.parent_class_name}& parent{delim}
      {params}) {{
    return std::unique_ptr<{self.table_name}>(new {self.table_name}(
        parent.string_pool(), parent, RowMap(0, parent.row_count()){delim}
        {args}));
  }}

  static std::unique_ptr<{self.table_name}> SelectAndExtendParent(
      const {self.parent_class_name}& parent,
      std::vector<{self.parent_class_name}::RowNumber> parent_overlay{delim}
      {params}) {{
    std::vector<uint32_t> prs_untyped(parent_overlay.size());
    for (uint32_t i = 0; i < parent_overlay.size(); ++i) {{
      prs_untyped[i] = parent_overlay[i].row_number();
    }}
    return std::unique_ptr<{self.table_name}>(new {self.table_name}(
        parent.string_pool(), parent, RowMap(std::move(prs_untyped)){delim}
        {args}));
  }}
    '''

  def extend_constructor(self) -> str:
    if not self.table.parent:
      return ''
    storage_layer_init = self.foreach_col(
        ColumnSerializer.storage_layer_init, delimiter=',\n        ')
    storage_layer_sep = '\n,' if storage_layer_init else ''
    null_layer_init = self.foreach_col(
        ColumnSerializer.null_layer_init, delimiter=',\n        ')
    null_layer_sep = '\n,' if null_layer_init else ''
    params = self.foreach_col(
        ColumnSerializer.extend_parent_param, delimiter='\n, ')
    storage_layer_create = self.foreach_col(
        ColumnSerializer.storage_layer_create, delimiter=',')
    null_layer_create = self.foreach_col(
        ColumnSerializer.null_layer_create, delimiter=',')
    return f'''
  {self.table_name}(StringPool* pool,
            const {self.parent_class_name}& parent,
            const RowMap& parent_overlay{',' if params else ''}
            {params})
      : macros_internal::MacroTable(
          pool,
          GetColumns(this, &parent),
          parent,
          parent_overlay),
          const_parent_(&parent){storage_layer_sep}
        {storage_layer_init}{null_layer_sep}
        {null_layer_init} {{
    {self.foreach_col(ColumnSerializer.static_assert_flags)}
    {self.foreach_col(ColumnSerializer.extend_nullable_vector)}

    std::vector<RefPtr<column::OverlayLayer>> overlay_layers(OverlayCount(&parent) + 1);
    for (uint32_t i = 0; i < overlay_layers.size(); ++i) {{
      if (overlays()[i].row_map().IsIndexVector()) {{
        overlay_layers[i].reset(new column::ArrangementOverlay(
            overlays()[i].row_map().GetIfIndexVector(),
            column::DataLayerChain::Indices::State::kNonmonotonic));
      }} else if (overlays()[i].row_map().IsBitVector()) {{
        overlay_layers[i].reset(new column::SelectorOverlay(
            overlays()[i].row_map().GetIfBitVector()));
      }} else if (overlays()[i].row_map().IsRange()) {{
        overlay_layers[i].reset(new column::RangeOverlay(
            overlays()[i].row_map().GetIfIRange()));
      }}
    }}

    OnConstructionCompleted(
      {{{storage_layer_create}}}, {{{null_layer_create}}}, std::move(overlay_layers));
  }}
    '''

  def column_count(self) -> str:
    return str(len(self.column_serializers))

  def serialize(self) -> str:
    return f'''
class {self.table_name} : public macros_internal::MacroTable {{
 public:
  static constexpr uint32_t kColumnCount = {self.column_count().strip()};

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
  static_assert(std::is_trivially_destructible_v<RowNumber>,
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
    return ConstIterator(this, Table::IterateRows());
  }}

  Iterator IterateRows() {{ return Iterator(this, Table::IterateRows()); }}

  ConstIterator FilterToIterator(const Query& q) const {{
    return ConstIterator(this, QueryToIterator(q));
  }}

  Iterator FilterToIterator(const Query& q) {{
    return Iterator(this, QueryToIterator(q));
  }}

  void ShrinkToFit() {{
    {self.foreach_col(ColumnSerializer.shrink_to_fit)}
  }}

  ConstRowReference operator[](uint32_t r) const {{
    return ConstRowReference(this, r);
  }}
  RowReference operator[](uint32_t r) {{ return RowReference(this, r); }}
  ConstRowReference operator[](RowNumber r) const {{
    return ConstRowReference(this, r.row_number());
  }}
  RowReference operator[](RowNumber r) {{
    return RowReference(this, r.row_number());
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
    return IdAndRow{{id, row_number, RowReference(this, row_number),
                     RowNumber(row_number)}};
  }}

  {self.extend().strip()}

  {self.foreach_col(ColumnSerializer.accessor)}

  {self.foreach_col(ColumnSerializer.mutable_accessor)}

 private:
  {self.extend_constructor().strip()}
  {self.parent_field().strip()}
  {self.foreach_col(ColumnSerializer.storage)}

  {self.foreach_col(ColumnSerializer.storage_layer)}

  {self.foreach_col(ColumnSerializer.null_layer)}
}};
  '''.strip('\n')


def serialize_header(ifdef_guard: str, tables: List[ParsedTable],
                     include_paths: List[str]) -> str:
  """Serializes a table header file containing the given set of tables."""
  # Replace the backslash with forward slash when building on Windows.
  # Caused b/327985369 without the replace.
  include_paths_str = '\n'.join([f'#include "{i}"' for i in include_paths
                                ]).replace("\\", "/")
  tables_str = '\n\n'.join([TableSerializer(t).serialize() for t in tables])
  return f'''
#ifndef {ifdef_guard}
#define {ifdef_guard}

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/arrangement_overlay.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/dense_null_overlay.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/id_storage.h"
#include "src/trace_processor/db/column/null_overlay.h"
#include "src/trace_processor/db/column/range_overlay.h"
#include "src/trace_processor/db/column/selector_overlay.h"
#include "src/trace_processor/db/column/set_id_storage.h"
#include "src/trace_processor/db/column/string_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/db/typed_column.h"
#include "src/trace_processor/db/typed_column_internal.h"
#include "src/trace_processor/tables/macros_internal.h"

{include_paths_str}

namespace perfetto::trace_processor::tables {{

{tables_str.strip()}

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
    flags.append('ColumnLegacy::Flag::kSorted')
  if ColumnFlag.HIDDEN in raw_flag:
    flags.append('ColumnLegacy::Flag::kHidden')
  if ColumnFlag.DENSE in raw_flag:
    flags.append('ColumnLegacy::Flag::kDense')
  if ColumnFlag.SET_ID in raw_flag:
    flags.append('ColumnLegacy::Flag::kSetId')
  return ' | '.join(flags)
