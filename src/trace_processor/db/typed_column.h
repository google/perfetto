/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_DB_TYPED_COLUMN_H_
#define SRC_TRACE_PROCESSOR_DB_TYPED_COLUMN_H_

#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/typed_column_internal.h"

namespace perfetto {
namespace trace_processor {

// TypedColumn<T>
//
// Introduction:
// TypedColumn exists to allow efficient access to the data in a Column without
// having to go through dynamic type checking. There are two main reasons for
// this:
// 1. Performance: dynamic type checking is not free and so if this is used
//    in a particularily hot codepath, the typechecking can be a significant
//    overhead.
// 2. Ergonomics: having to convert back and forth from/to SqlValue causes
//    signifcant clutter in parts of the code which can already be quite hard
//    to follow (e.g. trackers like SequenceStackProfileTracker which perform
//    cross checking of various ids).
//
// Implementation:
// TypedColumn is implemented as a memberless subclass of Column. This allows
// us to static cast from a Column* to a TypedColumn<T> where we know the
// type T. The methods of TypedColumn are type-specialized methods of Column
// which allow callers to pass real types instead of using SqlValue.
//
// There are two helper classes (tc_internal::TypeHandler and
// tc_internal::Serializer) where we specialize behaviour which needs to be
// different based on T. See their class documentation and below for details
// on their purpose.
template <typename T>
class TypedColumn : public Column {
 private:
  using TH = tc_internal::TypeHandler<T>;

 public:
  // The type of the data in this column.
  using type = T;

  // The non-optional type of the data in this column.
  using non_optional_type = typename TH::non_optional_type;

  // The type which should be passed to SqlValue functions.
  using sql_value_type = typename TH::sql_value_type;

  // The type of which is actually stored inside ColumnStorage. Can be different
  // from T because we treat table ids to just be uint32_t inside the storage
  // (handling ids would add an extra type to consider when filtering for no
  // benefit.
  using stored_type = typename TH::stored_type;

 private:
  using Serializer = tc_internal::Serializer<non_optional_type>;

 public:
  T operator[](uint32_t row) const { return GetAtIdx(overlay().Get(row)); }

  // Special function only for string types to allow retrieving the string
  // directly from the column.
  template <bool is_string = TH::is_string>
  typename std::enable_if<is_string, NullTermStringView>::type GetString(
      uint32_t row) const {
    return GetStringAtIdx(overlay().Get(row));
  }

  // Sets the data in the column at index |row|.
  void Set(uint32_t row, non_optional_type v) {
    SetAtIdx(overlay().Get(row), v);
  }

  // Inserts the value at the end of the column.
  void Append(T v) { mutable_storage()->Append(Serializer::Serialize(v)); }

  // Returns the row containing the given value in the Column.
  base::Optional<uint32_t> IndexOf(sql_value_type v) const {
    return Column::IndexOf(ToSqlValue(v));
  }

  std::vector<T> ToVectorForTesting() const {
    std::vector<T> result(overlay().size());
    for (uint32_t i = 0; i < overlay().size(); ++i)
      result[i] = (*this)[i];
    return result;
  }

  // Helper functions to create constraints for the given value.
  Constraint eq(sql_value_type v) const { return eq_value(ToSqlValue(v)); }
  Constraint gt(sql_value_type v) const { return gt_value(ToSqlValue(v)); }
  Constraint lt(sql_value_type v) const { return lt_value(ToSqlValue(v)); }
  Constraint ne(sql_value_type v) const { return ne_value(ToSqlValue(v)); }
  Constraint ge(sql_value_type v) const { return ge_value(ToSqlValue(v)); }
  Constraint le(sql_value_type v) const { return le_value(ToSqlValue(v)); }

  // Implements equality between two items of type |T|.
  static constexpr bool Equals(T a, T b) { return TH::Equals(a, b); }

  // Encodes the default flags for a column of the current type.
  static constexpr uint32_t default_flags() {
    return TH::is_optional ? Flag::kNoFlag : Flag::kNonNull;
  }

  // Converts the static type T into the dynamic SqlValue type of this column.
  static SqlValue::Type SqlValueType() {
    return Column::ToSqlValueType<stored_type>();
  }

  // Cast a Column to TypedColumn or crash if that is unsafe.
  static TypedColumn<T>* FromColumn(Column* column) {
    return FromColumnInternal<TypedColumn<T>>(column);
  }

  // Cast a Column to TypedColumn or crash if that is unsafe.
  static const TypedColumn<T>* FromColumn(const Column* column) {
    return FromColumnInternal<const TypedColumn<T>>(column);
  }

  // Public for use by macro tables.
  void SetAtIdx(uint32_t idx, non_optional_type v) {
    auto serialized = Serializer::Serialize(v);
    mutable_storage()->Set(idx, serialized);
  }

  // Public for use by macro tables.
  T GetAtIdx(uint32_t idx) const {
    return Serializer::Deserialize(TH::Get(storage(), idx));
  }

  template <bool is_string = TH::is_string>
  typename std::enable_if<is_string, NullTermStringView>::type GetStringAtIdx(
      uint32_t idx) const {
    return string_pool().Get(storage().Get(idx));
  }

 private:
  friend class Table;

  template <typename Output, typename Input>
  static Output* FromColumnInternal(Input* column) {
    // While casting from a base to derived without constructing as a derived is
    // technically UB, in practice, this is at the heart of protozero (see
    // Message::BeginNestedMessage) so we use it here.
    static_assert(sizeof(TypedColumn<T>) == sizeof(Column),
                  "TypedColumn cannot introduce extra state.");

    if (column->template IsColumnType<stored_type>() &&
        (column->IsNullable() == TH::is_optional) && !column->IsId()) {
      return static_cast<Output*>(column);
    } else {
      PERFETTO_FATAL("Unsafe to convert Column TypedColumn (%s)",
                     column->name());
    }
  }

  const ColumnStorage<stored_type>& storage() const {
    return Column::storage<stored_type>();
  }
  ColumnStorage<stored_type>* mutable_storage() {
    return Column::mutable_storage<stored_type>();
  }
};

// Represents a column containing ids.
template <typename Id>
class IdColumn : public Column {
 public:
  // The type of the data in this column.
  using type = Id;

  // The underlying type used when comparing ids.
  using stored_type = uint32_t;

  Id operator[](uint32_t row) const { return Id(overlay().Get(row)); }

  base::Optional<uint32_t> IndexOf(Id id) const {
    return overlay().RowOf(id.value);
  }

  // Public for use by macro tables.
  Id GetAtIdx(uint32_t idx) const { return Id(idx); }

  // Static cast a Column to IdColumn or crash if that is likely to be
  // unsafe.
  static const IdColumn<Id>* FromColumn(const Column* column) {
    // While casting from a base to derived without constructing as a derived is
    // technically UB, in practice, this is at the heart of protozero (see
    // Message::BeginNestedMessage) so we use it here.
    static_assert(sizeof(IdColumn<Id>) == sizeof(Column),
                  "TypedColumn cannot introduce extra state.");

    if (column->IsId()) {
      return static_cast<const IdColumn<Id>*>(column);
    } else {
      PERFETTO_FATAL("Unsafe to convert Column to IdColumn (%s)",
                     column->name());
    }
  }

  // Helper functions to create constraints for the given value.
  Constraint eq(uint32_t v) const { return eq_value(SqlValue::Long(v)); }
  Constraint gt(uint32_t v) const { return gt_value(SqlValue::Long(v)); }
  Constraint lt(uint32_t v) const { return lt_value(SqlValue::Long(v)); }
  Constraint ne(uint32_t v) const { return ne_value(SqlValue::Long(v)); }
  Constraint ge(uint32_t v) const { return ge_value(SqlValue::Long(v)); }
  Constraint le(uint32_t v) const { return le_value(SqlValue::Long(v)); }

 private:
  friend class Table;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_TYPED_COLUMN_H_
