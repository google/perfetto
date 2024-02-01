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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_SET_ID_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_SET_ID_STORAGE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

// Storage for SetId columns.
class SetIdStorage final : public Column {
 public:
  using SetId = uint32_t;

  explicit SetIdStorage(const std::vector<uint32_t>* data) : values_(data) {}

  SearchValidationResult ValidateSearchConstraints(SqlValue,
                                                   FilterOp) const override;

  RangeOrBitVector Search(FilterOp, SqlValue, Range) const override;

  RangeOrBitVector IndexSearch(FilterOp, SqlValue, Indices) const override;

  Range OrderedIndexSearch(FilterOp, SqlValue, Indices) const override;

  void StableSort(uint32_t* rows, uint32_t rows_size) const override;

  void Sort(uint32_t* rows, uint32_t rows_size) const override;

  void Serialize(StorageProto*) const override;

  uint32_t size() const override {
    return static_cast<uint32_t>(values_->size());
  }

  std::string DebugString() const override { return "SetIdStorage"; }

 private:
  BitVector IndexSearch(FilterOp, SetId, uint32_t*, uint32_t) const;
  Range BinarySearchIntrinsic(FilterOp, SetId, Range search_range) const;

  // TODO(b/307482437): After the migration vectors should be owned by storage,
  // so change from pointer to value.
  const std::vector<SetId>* values_ = nullptr;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_SET_ID_STORAGE_H_
