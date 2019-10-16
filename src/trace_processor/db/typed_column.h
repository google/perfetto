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

// Represents a column containing data with the given type T.
//
// This class exists as a memberless subclass of Column (i.e. sizeof(Column) ==
// sizeof(TypedColumn<T>)); this is because Columns are type erased but we still
// want low boilerplate methods to get/set rows in columns where we know the
// type.
template <typename T>
struct TypedColumn : public Column {
  using StoredType = T;

  T operator[](uint32_t row) const { return *GetTyped<T>(row); }
  void Set(uint32_t row, T value) { SetTyped(row, value); }
  void Append(T value) { return mutable_sparse_vector<T>()->Append(value); }
};

template <typename T>
struct TypedColumn<base::Optional<T>> : public Column {
  using StoredType = T;

  base::Optional<T> operator[](uint32_t row) const { return GetTyped<T>(row); }
  void Set(uint32_t row, T value) { SetTyped(row, value); }

  void Append(base::Optional<T> value) {
    return mutable_sparse_vector<T>()->Append(value);
  }
};

template <>
struct TypedColumn<StringPool::Id> : public Column {
  using StoredType = StringPool::Id;

  StringPool::Id operator[](uint32_t row) const {
    return *GetTyped<StringPool::Id>(row);
  }
  NullTermStringView GetString(uint32_t row) const {
    return GetStringPoolString(row);
  }
  void Set(uint32_t row, StringPool::Id value) { SetTyped(row, value); }
  void Append(StringPool::Id value) {
    return mutable_sparse_vector<StringPool::Id>()->Append(value);
  }
};

template <>
struct TypedColumn<base::Optional<StringPool::Id>>
    : public TypedColumn<StringPool::Id> {
  void Append(base::Optional<StringPool::Id> value) {
    // Since StringPool::Id == 0 is always treated as null, rewrite
    // base::nullopt -> 0 to remove an extra check at filter time for
    // base::nullopt. Instead, that code can assume that the SparseVector layer
    // always returns a valid id and can handle the nullability at the
    // stringpool level.
    // TODO(lalitm): remove this special casing if we migrate all tables over
    // to macro tables and find that we can remove support for null stringids
    // in the stringpool.
    return TypedColumn<StringPool::Id>::Append(value ? *value
                                                     : StringPool::Id(0u));
  }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_TYPED_COLUMN_H_
