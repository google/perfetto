/*
 * Copyright (C) 2023 The Android Open Source Project
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
#ifndef SRC_TRACE_PROCESSOR_DB_NUMERIC_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_NUMERIC_STORAGE_H_

#include <variant>
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/storage.h"
#include "src/trace_processor/db/storage_variants.h"

namespace perfetto {
namespace trace_processor {
namespace column {

class NumericStorage : public Storage {
 public:
  NumericStorage(const void* data, ColumnType type)
      : type_(type), data_(data) {}

  void StableSort(std::vector<uint32_t>&) const override;

  void CompareFast(FilterOp op,
                   SqlValue val,
                   const void* start,
                   uint32_t num_elements,
                   BitVector::Builder& builder) const override;

  // Inefficiently compares series of |num_elements| of data from |data_start|
  // to comparator value and appends results to BitVector::Builder. Should be
  // avoided if possible, with `FastSeriesComparison` used instead.
  void CompareSlow(FilterOp op,
                   SqlValue val,
                   const void* start,
                   uint32_t num_elements,
                   BitVector::Builder& builder) const override;

  // Compares sorted (asc) series of |num_elements| of data from |data_start| to
  // comparator value. Should be used where possible.
  void CompareSorted(FilterOp op,
                     SqlValue val,
                     const void* data_start,
                     uint32_t num_elements,
                     RowMap&) const override;

 private:
  const ColumnType type_;
  const void* data_;
};

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_NUMERIC_STORAGE_H_
