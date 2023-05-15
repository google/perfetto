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
  NumericStorage(void* data, uint32_t size, ColumnType type)
      : type_(type), data_(data), size_(size) {}

  void StableSort(uint32_t* rows, uint32_t rows_size) const override;

  void CompareFast(FilterOp op,
                   SqlValue val,
                   uint32_t offset,
                   uint32_t num_elements,
                   BitVector::Builder& builder) const override;

  void CompareSlow(FilterOp op,
                   SqlValue val,
                   uint32_t offset,
                   uint32_t num_elements,
                   BitVector::Builder& builder) const override;

  void CompareSorted(FilterOp op, SqlValue val, RowMap&) const override;

  void CompareSortedIndexes(FilterOp op,
                            SqlValue val,
                            uint32_t* order,
                            RowMap&) const override;

  uint32_t size() const override { return size_; }

 private:
  // As we don't template those functions, we need to use std::visitor to type
  // `start`, hence this wrapping.
  uint32_t UpperBoundIndex(NumericValue val) const;

  // As we don't template those functions, we need to use std::visitor to type
  // `start`, hence this wrapping.
  uint32_t LowerBoundIndex(NumericValue val) const;

  // As we don't template those functions, we need to use std::visitor to type
  // `start`, hence this wrapping.
  uint32_t UpperBoundIndex(NumericValue val, uint32_t* order) const;

  // As we don't template those functions, we need to use std::visitor to type
  // `start`, hence this wrapping.
  uint32_t LowerBoundIndex(NumericValue val, uint32_t* order) const;

  const ColumnType type_;
  const void* data_;
  const uint32_t size_;
};

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_NUMERIC_STORAGE_H_
