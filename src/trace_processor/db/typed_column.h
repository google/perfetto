/*
 * Copyright (C) 2019 The Android Open Source Project
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

namespace perfetto {
namespace trace_processor {

// Represents a column containing ids.
struct IdColumn : public Column {
  Constraint eq(uint32_t v) const { return eq_value(SqlValue::Long(v)); }
  Constraint gt(uint32_t v) const { return gt_value(SqlValue::Long(v)); }
  Constraint lt(uint32_t v) const { return lt_value(SqlValue::Long(v)); }
  Constraint ne(uint32_t v) const { return ne_value(SqlValue::Long(v)); }
  Constraint ge(uint32_t v) const { return ge_value(SqlValue::Long(v)); }
  Constraint le(uint32_t v) const { return le_value(SqlValue::Long(v)); }
};

// Represents a column containing data with the given type T.
//
// This class exists as a memberless subclass of Column (i.e. sizeof(Column) ==
// sizeof(TypedColumn<T>)); this is because Columns are type erased but we still
// want low boilerplate methods to get/set rows in columns where we know the
// type.
template <typename T>
struct TypedColumn : public Column {
  using StoredType = T;

  T operator[](uint32_t row) const {
    return sparse_vector<T>().GetNonNull(row_map().Get(row));
  }
  void Set(uint32_t row, T v) {
    mutable_sparse_vector<T>()->Set(row_map().Get(row), v);
  }
  void Append(T v) { mutable_sparse_vector<T>()->Append(v); }

  Constraint eq(T v) const { return eq_value(NumericToSqlValue(v)); }
  Constraint gt(T v) const { return gt_value(NumericToSqlValue(v)); }
  Constraint lt(T v) const { return lt_value(NumericToSqlValue(v)); }
  Constraint ne(T v) const { return ne_value(NumericToSqlValue(v)); }
  Constraint ge(T v) const { return ge_value(NumericToSqlValue(v)); }
  Constraint le(T v) const { return le_value(NumericToSqlValue(v)); }

  static constexpr uint32_t default_flags() { return Flag::kNonNull; }
};

template <typename T>
struct TypedColumn<base::Optional<T>> : public Column {
  using StoredType = T;

  base::Optional<T> operator[](uint32_t row) const {
    return sparse_vector<T>().Get(row_map().Get(row));
  }
  void Set(uint32_t row, T v) {
    mutable_sparse_vector<T>()->Set(row_map().Get(row), v);
  }
  void Append(base::Optional<T> v) { mutable_sparse_vector<T>()->Append(v); }

  Constraint eq(T v) const { return eq_value(NumericToSqlValue(v)); }
  Constraint gt(T v) const { return gt_value(NumericToSqlValue(v)); }
  Constraint lt(T v) const { return lt_value(NumericToSqlValue(v)); }
  Constraint ne(T v) const { return ne_value(NumericToSqlValue(v)); }
  Constraint ge(T v) const { return ge_value(NumericToSqlValue(v)); }
  Constraint le(T v) const { return le_value(NumericToSqlValue(v)); }

  static constexpr uint32_t default_flags() { return Flag::kNoFlag; }
};

template <>
struct TypedColumn<StringPool::Id> : public Column {
  using StoredType = StringPool::Id;

  StringPool::Id operator[](uint32_t row) const {
    return sparse_vector<StringPool::Id>().GetNonNull(row_map().Get(row));
  }
  NullTermStringView GetString(uint32_t row) const {
    return GetStringPoolStringAtIdx(row_map().Get(row));
  }
  void Set(uint32_t row, StringPool::Id v) {
    mutable_sparse_vector<StringPool::Id>()->Set(row_map().Get(row), v);
  }
  void Append(StringPool::Id v) {
    mutable_sparse_vector<StringPool::Id>()->Append(v);
  }

  Constraint eq(const char* v) const { return eq_value(SqlValue::String(v)); }
  Constraint gt(const char* v) const { return gt_value(SqlValue::String(v)); }
  Constraint lt(const char* v) const { return lt_value(SqlValue::String(v)); }
  Constraint ne(const char* v) const { return ne_value(SqlValue::String(v)); }
  Constraint ge(const char* v) const { return ge_value(SqlValue::String(v)); }
  Constraint le(const char* v) const { return le_value(SqlValue::String(v)); }

  static constexpr uint32_t default_flags() { return Flag::kNonNull; }
};

template <>
struct TypedColumn<base::Optional<StringPool::Id>>
    : public TypedColumn<StringPool::Id> {
  void Append(base::Optional<StringPool::Id> v) {
    // Since StringPool::Id == 0 is always treated as null, rewrite
    // base::nullopt -> 0 to remove an extra check at filter time for
    // base::nullopt. Instead, that code can assume that the SparseVector layer
    // always returns a valid id and can handle the nullability at the
    // stringpool level.
    // TODO(lalitm): remove this special casing if we migrate all tables over
    // to macro tables and find that we can remove support for null stringids
    // in the stringpool.
    return TypedColumn<StringPool::Id>::Append(v ? *v : StringPool::Id(0u));
  }

  static constexpr uint32_t default_flags() { return Flag::kNonNull; }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_TYPED_COLUMN_H_
