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
};

template <typename T>
struct TypedColumn<base::Optional<T>> : public Column {
  using StoredType = T;

  base::Optional<T> operator[](uint32_t row) const { return GetTyped<T>(row); }
};

template <>
struct TypedColumn<StringPool::Id> : public Column {
  using StoredType = StringPool::Id;

  NullTermStringView operator[](uint32_t row) const { return GetString(row); }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_TYPED_COLUMN_H_
