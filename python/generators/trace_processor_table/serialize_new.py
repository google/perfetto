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
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import SqlAccess
from python.generators.trace_processor_table.util import ParsedTable
from python.generators.trace_processor_table.util import ParsedColumn
from python.generators.trace_processor_table.util import parse_type


class ColumnSerializer:

  def __init__(self, table: ParsedTable, column: ParsedColumn, col_index: int):
    self.col_index = col_index
    self.parsed_col = column
    self.col = self.parsed_col.column
    self.name = self.col.name
    self.flags = self.col.flags

    parsed_type = parse_type(table.table, self.col.type)

    self.cpp_type_non_optional = parsed_type.cpp_type()
    self.cpp_type_with_optionality = parsed_type.cpp_type_with_optionality()
    self.is_optional = parsed_type.is_optional
    self.is_string = self.cpp_type_non_optional == 'StringPool::Id'
    self.is_id_type = parsed_type.id_table
    self.is_no_transform_id = parsed_type.is_no_transform_id()

    self.is_implicit_id = self.parsed_col.is_implicit_id

  def row_field(self) -> Optional[str]:
    if self.is_implicit_id:
      return None
    return f'    {self.cpp_type_with_optionality} {self.name};'

  def row_insert_arg_value(self) -> Optional[str]:
    """Generates the C++ code to access this column's value from a Row object,
    for use in the Dataframe::InsertUnchecked call."""
    if self.is_implicit_id:
      return f'std::monostate()'
    if self.is_optional and self.is_id_type and not self.is_no_transform_id:
      return f'row.{self.name} ? std::make_optional(row.{self.name}->value) : std::nullopt'
    if self.is_optional and self.is_string:
      return f'row.{self.name} && row.{self.name} != StringPool::Id::Null() ? std::make_optional(*row.{self.name}) : std::nullopt'
    if self.is_string:
      return f'row.{self.name} != StringPool::Id::Null() ? std::make_optional(row.{self.name}) : std::nullopt'
    if self.is_id_type and not self.is_no_transform_id:
      return f'row.{self.name}.value'
    return f'row.{self.name}'

  def cursor_getter(self) -> Optional[str]:
    if self.col.cpp_access == CppAccess.NONE:
      return ''

    if self.col.cpp_access_duration == CppAccessDuration.PRE_FINALIZATION_ONLY:
      dcheck = 'PERFETTO_DCHECK(!dataframe_->finalized());'
    else:
      dcheck = ''

    if self.is_optional and self.is_id_type:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        auto res = cursor_.GetCellUnchecked<ColumnIndex::{self.name}>(kSpec);
        return res ? std::make_optional({self.cpp_type_non_optional}{{*res}}) : std::nullopt;
      }}'''
    if self.is_optional and self.is_string:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        auto res = cursor_.GetCellUnchecked<ColumnIndex::{self.name}>(kSpec);
        return res && res != StringPool::Id::Null() ? std::make_optional({self.cpp_type_non_optional}{{*res}}) : std::nullopt;
      }}'''
    if self.is_string:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        auto res = cursor_.GetCellUnchecked<ColumnIndex::{self.name}>(kSpec);
        return res && res != StringPool::Id::Null() ? *res : StringPool::Id::Null();
      }}'''
    if self.is_id_type:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        return {self.cpp_type_non_optional}{{cursor_.GetCellUnchecked<ColumnIndex::{self.name}>(kSpec)}};
      }}'''
    return f'''
    {self.cpp_type_with_optionality} {self.name}() const {{
      {dcheck}
      return cursor_.GetCellUnchecked<ColumnIndex::{self.name}>(kSpec);
    }}'''

  def cursor_setter(self) -> Optional[str]:
    if self.col.cpp_access == CppAccess.NONE or self.col.cpp_access == CppAccess.READ:
      return ''
    if self.col.cpp_access_duration == CppAccessDuration.PRE_FINALIZATION_ONLY:
      dcheck = 'PERFETTO_DCHECK(!dataframe_->finalized());'
    else:
      dcheck = ''
    if self.is_optional and self.is_id_type and not self.is_no_transform_id:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        auto res_value = res ? std::make_optional(res->value) : std::nullopt;
        cursor_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, res_value);
      }}'''
    if self.is_optional and self.is_string:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        auto res_value = res && res != StringPool::Id::Null() ? std::make_optional(*res) : std::nullopt;
        cursor_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, res_value);
    }}'''
    if self.is_string:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        auto res_value = res != StringPool::Id::Null() ? std::make_optional(res) : std::nullopt;
        cursor_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, res_value);
    }}'''
    if self.is_id_type and not self.is_no_transform_id:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        cursor_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, res.value);
      }}'''
    return f'''
    void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
      cursor_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, res);
    }}'''

  def row_reference_getter(self) -> str:
    if self.col.cpp_access == CppAccess.NONE:
      return ''
    if self.col.cpp_access_duration == CppAccessDuration.PRE_FINALIZATION_ONLY:
      dcheck = 'PERFETTO_DCHECK(!table_->dataframe_.finalized());'
    else:
      dcheck = ''
    if self.is_optional and self.is_id_type:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        auto res = table_->dataframe_.template GetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_);
        return res ? std::make_optional({self.cpp_type_non_optional}{{*res}}) : std::nullopt;
      }}'''
    if self.is_optional and self.is_string:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        auto res = table_->dataframe_.template GetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_);
        return res && res != StringPool::Id::Null() ? std::make_optional({self.cpp_type_non_optional}{{*res}}) : std::nullopt;
      }}'''
    if self.is_string:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        auto res = table_->dataframe_.template GetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_);
        return res && res != StringPool::Id::Null() ? {self.cpp_type_non_optional}{{*res}} : StringPool::Id::Null();
      }}'''
    if self.is_id_type:
      return f'''
      {self.cpp_type_with_optionality} {self.name}() const {{
        {dcheck}
        return {self.cpp_type_non_optional}{{table_->dataframe_.template GetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_)}};
      }}'''
    return f'''
    {self.cpp_type_with_optionality} {self.name}() const {{
      {dcheck}
      return table_->dataframe_.template GetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_);
    }}'''

  def row_reference_setter(self) -> Optional[str]:
    if self.col.cpp_access == CppAccess.NONE or self.col.cpp_access == CppAccess.READ:
      return ''
    if self.col.cpp_access_duration == CppAccessDuration.PRE_FINALIZATION_ONLY:
      dcheck = 'PERFETTO_DCHECK(!table_->dataframe_.finalized());'
    else:
      dcheck = ''
    if self.is_optional and self.is_id_type and not self.is_no_transform_id:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        auto res_value = res ? std::make_optional(res->value) : std::nullopt;
        table_->dataframe_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_, res_value);
      }}'''
    if self.is_optional and self.is_string:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        auto res_value = res && res != StringPool::Id::Null() ? std::make_optional(*res) : std::nullopt;
        table_->dataframe_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_, res_value);
    }}'''
    if self.is_string:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        auto res_value = res != StringPool::Id::Null() ? std::make_optional(res) : std::nullopt;
        table_->dataframe_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_, res_value);
    }}'''
    if self.is_id_type and not self.is_no_transform_id:
      return f'''
      void set_{self.name}({self.cpp_type_with_optionality} res) {{
        {dcheck}
        table_->dataframe_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_, res.value);
      }}'''
    return f'''
    void set_{self.name}({self.cpp_type_with_optionality} res) {{
      {dcheck}
      table_->dataframe_.SetCellUnchecked<ColumnIndex::{self.name}>(kSpec, row_, res);
    }}'''

  def colindex_member(self) -> str:
    return f'static constexpr uint32_t {self.name} = {self.col_index}'

  def typespec_column_name_literal(self) -> str:
    return f'"{self.name}"'

  def _get_storage_type_tag(self) -> str:
    if self.is_implicit_id:
      return 'dataframe::Id'
    if self.is_string:
      return 'dataframe::String'
    if self.cpp_type_non_optional == 'uint32_t' or self.is_id_type:
      return 'dataframe::Uint32'
    if self.cpp_type_non_optional == 'int32_t':
      return 'dataframe::Int32'
    if self.cpp_type_non_optional == 'int64_t':
      return 'dataframe::Int64'
    if self.cpp_type_non_optional == 'double':
      return 'dataframe::Double'
    raise ValueError(
        f"Unknown cpp_type_non_optional '{self.cpp_type_non_optional}' for Dataframe StorageType for column '{self.name}'"
    )

  def _get_nullability_tag(self) -> str:
    if self.is_optional or self.is_string:
      if self.col.sql_access == SqlAccess.HIGH_PERF:
        return 'dataframe::DenseNull'
      if self.col.cpp_access == CppAccess.NONE:
        return 'dataframe::SparseNull'
      if self.col.cpp_access == CppAccess.READ_AND_HIGH_PERF_WRITE:
        return 'dataframe::DenseNull'
      assert self.col.cpp_access in (
          CppAccess.READ,
          CppAccess.READ_AND_LOW_PERF_WRITE,
      )
      # TODO(lalitm): we force this to dense for strings because in the
      # pre-dataframe days, string nullability was just represented by
      # StringPool::Id::Null(). There are hidden dependencies everywhere in the
      # codebase that expect O(1) writes to string columns. Fixing this is
      # non-trivial, so we just keep it dense for now.
      if self.is_string and self.col.cpp_access == CppAccess.READ_AND_LOW_PERF_WRITE:
        return 'dataframe::DenseNull'
      if self.col.cpp_access_duration == CppAccessDuration.PRE_FINALIZATION_ONLY:
        return 'dataframe::SparseNullWithPopcountUntilFinalization'
      assert self.col.cpp_access_duration == CppAccessDuration.POST_FINALIZATION
      return 'dataframe::SparseNullWithPopcountAlways'
    return 'dataframe::NonNull'

  def _get_sort_state_tag(self) -> str:
    if ColumnFlag.SORTED in self.flags:
      if self.is_implicit_id:
        return 'dataframe::IdSorted'
      if ColumnFlag.SET_ID in self.flags:
        return 'dataframe::SetIdSorted'
      return 'dataframe::Sorted'
    return 'dataframe::Unsorted'

  def _get_duplicate_state_tag(self) -> str:
    if self.is_implicit_id:
      return 'dataframe::NoDuplicates'
    return 'dataframe::HasDuplicates'

  def typespec_typed_column_spec_expr(self) -> str:
    storage_tag = self._get_storage_type_tag()
    null_tag = self._get_nullability_tag()
    sort_tag = self._get_sort_state_tag()
    dupe_tag = self._get_duplicate_state_tag()
    return (
        f"dataframe::CreateTypedColumnSpec("
        f"{storage_tag}{{}}, {null_tag}{{}}, {sort_tag}{{}}, {dupe_tag}{{}})")


class TableSerializer(object):

  def __init__(self, parsed: ParsedTable):
    self.table = parsed.table
    self.table_name = parsed.table.class_name
    self.column_serializers: List[ColumnSerializer] = []

    # Filter out aliases first, then assign col_index
    temp_serializers = []
    for c_parsed in parsed.columns:
      if isinstance(c_parsed.column.type, Alias):
        continue
      # col_index will be assigned when rebuilding self.column_serializers
      temp_serializers.append(ColumnSerializer(parsed, c_parsed, 0))

    self.column_serializers = [
        ColumnSerializer(parsed, cs.parsed_col, i)
        for i, cs in enumerate(temp_serializers)
    ]

  def foreach_col(self, serialize_fn, delimiter='\n') -> str:
    lines = []
    for c_ser in self.column_serializers:
      serialized = serialize_fn(c_ser)
      if serialized:
        lines.append(serialized.lstrip('\n').rstrip())
    return delimiter.join(lines).strip()

  def row_struct(self) -> str:
    fields = []
    constructor_params_list = []
    constructor_inits_list = []

    for c_ser in self.column_serializers:
      if not c_ser.is_implicit_id:
        fields.append(c_ser.row_field())
        constructor_params_list.append(
            f'{c_ser.cpp_type_with_optionality} _{c_ser.name} = {{}}')
        constructor_inits_list.append(f'{c_ser.name}(std::move(_{c_ser.name}))')

    fields_str = "\n".join(filter(None, fields))
    params_str = ', '.join(constructor_params_list)
    inits_str = (
        ': ' +
        ', '.join(constructor_inits_list)) if constructor_inits_list else ''

    return f'''
  struct Row {{
    Row({params_str}) {inits_str} {{}}

    bool operator==(const Row& other) const {{
      return std::tie({', '.join([f'{c_ser.name}' for c_ser in self.column_serializers if not c_ser.is_implicit_id])}) ==
             std::tie({', '.join([f'other.{c_ser.name}' for c_ser in self.column_serializers if not c_ser.is_implicit_id])});
    }}

    {fields_str}
  }};'''

  def serialize(self) -> str:
    col_names = self.foreach_col(
        ColumnSerializer.typespec_column_name_literal, delimiter=',')
    col_specs = self.foreach_col(
        ColumnSerializer.typespec_typed_column_spec_expr, delimiter=',\n    ')
    col_index = self.foreach_col(
        ColumnSerializer.colindex_member, delimiter=';\n    ')
    row_ref_getter = self.foreach_col(
        ColumnSerializer.row_reference_getter, delimiter='\n    ')
    row_ref_setter = self.foreach_col(
        ColumnSerializer.row_reference_setter, delimiter='\n    ')
    cursor_getter = self.foreach_col(
        ColumnSerializer.cursor_getter, delimiter='\n')
    cursor_setter = self.foreach_col(
        ColumnSerializer.cursor_setter, delimiter='\n')
    insert_argvalue = self.foreach_col(
        ColumnSerializer.row_insert_arg_value, delimiter=', ')
    return f'''
class {self.table_name} {{
 public:
  static constexpr auto kSpec = dataframe::CreateTypedDataframeSpec(
    {{{col_names}}},
    {col_specs});

  struct Id : BaseId {{
    Id() = default;
    explicit constexpr Id(uint32_t _value) : BaseId(_value) {{}}

    bool operator==(const Id& other) const {{
      return value == other.value;
    }}
  }};
  struct RowReference;
  struct ConstRowReference;
  struct RowNumber {{
   public:
    explicit constexpr RowNumber(uint32_t value) : value_(value) {{}}
    uint32_t row_number() const {{ return value_; }}

    RowReference ToRowReference({self.table_name}* table) const {{
      return RowReference(table, value_);
    }}
    ConstRowReference ToRowReference(const {self.table_name}& table) const {{
      return ConstRowReference(&table, value_);
    }}

    bool operator==(const RowNumber& other) const {{
      return value_ == other.value_;
    }}
    bool operator<(const RowNumber& other) const {{
      return value_ < other.value_;
    }}
   private:
    uint32_t value_;
  }};
  struct ColumnIndex {{
    {col_index};
  }};
  struct RowReference {{
   public:
    explicit RowReference({self.table_name}* table, uint32_t row)
        : table_(table), row_(row) {{
        base::ignore_result(table_);
    }}
    {row_ref_getter}
    {row_ref_setter}
    RowNumber ToRowNumber() const {{
      return RowNumber{{row_}};
    }}

   private:
    friend struct ConstRowReference;
    {self.table_name}* table_;
    uint32_t row_;
  }};
  struct ConstRowReference {{
   public:
    explicit ConstRowReference(const {self.table_name}* table, uint32_t row)
        : table_(table), row_(row) {{
        base::ignore_result(table_);
    }}
    ConstRowReference(const RowReference& other)
        : table_(other.table_), row_(other.row_) {{}}
    {row_ref_getter}
    RowNumber ToRowNumber() const {{
      return RowNumber{{row_}};
    }}
   private:
    const {self.table_name}* table_;
    uint32_t row_;
  }};
  class ConstCursor {{
   public:
    explicit ConstCursor(const dataframe::Dataframe& df,
                         std::vector<dataframe::FilterSpec> filters,
                         std::vector<dataframe::SortSpec> sorts)
      : dataframe_(&df), cursor_(&df, std::move(filters), std::move(sorts)) {{
      base::ignore_result(dataframe_);
    }}

    PERFETTO_ALWAYS_INLINE void Execute() {{ cursor_.ExecuteUnchecked(); }}
    PERFETTO_ALWAYS_INLINE bool Eof() const {{ return cursor_.Eof(); }}
    PERFETTO_ALWAYS_INLINE void Next() {{ cursor_.Next(); }}
    template <typename C>
    PERFETTO_ALWAYS_INLINE void SetFilterValueUnchecked(uint32_t index, C value) {{
      cursor_.SetFilterValueUnchecked(index, std::move(value));
    }}
    RowNumber ToRowNumber() const {{
      return RowNumber{{cursor_.RowIndex()}};
    }}
    void Reset() {{ cursor_.Reset(); }}
    {cursor_getter}

   private:
    const dataframe::Dataframe* dataframe_;
    dataframe::TypedCursor cursor_;
  }};
  class Cursor {{
   public:
    explicit Cursor(dataframe::Dataframe& df,
                    std::vector<dataframe::FilterSpec> filters,
                    std::vector<dataframe::SortSpec> sorts)
      : dataframe_(&df), cursor_(&df, std::move(filters), std::move(sorts)) {{
      base::ignore_result(dataframe_);
    }}

    PERFETTO_ALWAYS_INLINE void Execute() {{ cursor_.ExecuteUnchecked(); }}
    PERFETTO_ALWAYS_INLINE bool Eof() const {{ return cursor_.Eof(); }}
    PERFETTO_ALWAYS_INLINE void Next() {{ cursor_.Next(); }}
    template <typename C>
    PERFETTO_ALWAYS_INLINE void SetFilterValueUnchecked(uint32_t index, C value) {{
      cursor_.SetFilterValueUnchecked(index, std::move(value));
    }}
    RowNumber ToRowNumber() const {{
      return RowNumber{{cursor_.RowIndex()}};
    }}
    void Reset() {{ cursor_.Reset(); }}

    {cursor_getter}
    {cursor_setter}

   private:
    dataframe::Dataframe* dataframe_;
    dataframe::TypedCursor cursor_;
  }};
  class Iterator {{
    public:
      explicit Iterator({self.table_name}* table) : table_(table) {{
        base::ignore_result(table_);
      }}
      explicit operator bool() const {{ return row_ < table_->row_count(); }}
      Iterator& operator++() {{
        ++row_;
        return *this;
      }}
      RowNumber row_number() const {{
        return RowNumber{{row_}};
      }}
      RowReference ToRowReference() const {{
        return RowReference(table_, row_);
      }}
      {row_ref_getter}
      {row_ref_setter}

    private:
      {self.table_name}* table_;
      uint32_t row_ = 0;
  }};
  class ConstIterator {{
    public:
      explicit ConstIterator(const {self.table_name}* table) : table_(table) {{
        base::ignore_result(table_);
      }}
      explicit operator bool() const {{ return row_ < table_->row_count(); }}
      ConstIterator& operator++() {{
        ++row_;
        return *this;
      }}
      RowNumber row_number() const {{
        return RowNumber{{row_}};
      }}
      ConstRowReference ToRowReference() const {{
        return ConstRowReference(table_, row_);
      }}
      {row_ref_getter}

    private:
      const {self.table_name}* table_;
      uint32_t row_ = 0;
  }};
  struct IdAndRow {{
    Id id;
    RowNumber row_number;
    uint32_t row;
    RowReference row_reference;
  }};
  {self.row_struct()}

  explicit {self.table_name}(StringPool* pool)
      : dataframe_(dataframe::Dataframe::CreateFromTypedSpec(kSpec, pool)) {{}}

  IdAndRow Insert(const Row& row) {{
    uint32_t row_count = dataframe_.row_count();
    dataframe_.InsertUnchecked(kSpec, {insert_argvalue});
    return IdAndRow{{Id{{row_count}}, RowNumber{{row_count}}, row_count, RowReference(this, row_count)}};
  }}

  uint32_t row_count() const {{
    return dataframe_.row_count();
  }}

  std::optional<ConstRowReference> FindById(Id id) const {{
    return ConstRowReference(this, id.value);
  }}
  ConstRowReference operator[](uint32_t row) const {{
    return ConstRowReference(this, row);
  }}

  std::optional<RowReference> FindById(Id id) {{
    return RowReference(this, id.value);
  }}
  RowReference operator[](uint32_t row) {{
    return RowReference(this, row);
  }}

  ConstCursor CreateCursor(
      std::vector<dataframe::FilterSpec> filters = {{}},
      std::vector<dataframe::SortSpec> sorts = {{}}) const {{
    return ConstCursor(dataframe_, std::move(filters), std::move(sorts));
  }}
  Cursor CreateCursor(
      std::vector<dataframe::FilterSpec> filters = {{}},
      std::vector<dataframe::SortSpec> sorts = {{}}) {{
    return Cursor(dataframe_, std::move(filters), std::move(sorts));
  }}

  Iterator IterateRows() {{ return Iterator(this); }}
  ConstIterator IterateRows() const {{ return ConstIterator(this); }}

  void Finalize() {{ dataframe_.Finalize(); }}

  void Clear() {{ dataframe_.Clear(); }}

  static const char* Name() {{
    return "{self.table.sql_name}";
  }}

  dataframe::Dataframe& dataframe() {{
    return dataframe_;
  }}
  const dataframe::Dataframe& dataframe() const {{
    return dataframe_;
  }}

 private:
  dataframe::Dataframe dataframe_;
}};
'''


def serialize_header(ifdef_guard: str, tables: List[ParsedTable],
                     include_paths: List[str]) -> str:
  """Serializes a table header file containing the given set of tables."""
  # Replace the backslash with forward slash when building on Windows.
  # Caused b/327985369 without the replace.
  include_paths_str = '\n'.join([f'#include "{i}"' for i in include_paths
                                ]).replace("\\", "/")
  tables_str = '\n\n'.join([TableSerializer(t).serialize() for t in tables])

  return f'''\
#ifndef {ifdef_guard}
#define {ifdef_guard}

#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/tables/macros_internal.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/specs.h"

{include_paths_str}

namespace perfetto::trace_processor::tables {{

{tables_str}

}}  // namespace perfetto::trace_processor::tables

#endif  // {ifdef_guard}
'''
