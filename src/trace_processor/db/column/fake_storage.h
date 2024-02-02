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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_FAKE_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_FAKE_STORAGE_H_

#include <memory>
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto {
namespace trace_processor {
namespace column {

// Fake implementation of Storage for use in tests.
class FakeStorage final : public Column {
 public:
  SearchValidationResult ValidateSearchConstraints(SqlValue,
                                                   FilterOp) const override;

  RangeOrBitVector Search(FilterOp op,
                          SqlValue value,
                          Range range) const override;

  RangeOrBitVector IndexSearch(FilterOp op,
                               SqlValue value,
                               uint32_t* indices,
                               uint32_t indices_count,
                               bool sorted) const override;

  void StableSort(uint32_t* rows, uint32_t rows_size) const override;

  void Sort(uint32_t* rows, uint32_t rows_size) const override;

  void Serialize(StorageProto*) const override;

  static std::unique_ptr<Column> SearchAll(uint32_t size) {
    return std::unique_ptr<Column>(new FakeStorage(size, SearchStrategy::kAll));
  }

  static std::unique_ptr<Column> SearchNone(uint32_t size) {
    return std::unique_ptr<Column>(
        new FakeStorage(size, SearchStrategy::kNone));
  }

  static std::unique_ptr<Column> SearchSubset(uint32_t size, Range r) {
    std::unique_ptr<FakeStorage> storage(
        new FakeStorage(size, SearchStrategy::kRange));
    storage->range_ = r;
    return std::move(storage);
  }

  static std::unique_ptr<Column> SearchSubset(uint32_t size, BitVector bv) {
    std::unique_ptr<FakeStorage> storage(
        new FakeStorage(size, SearchStrategy::kBitVector));
    storage->bit_vector_ = std::move(bv);
    return std::move(storage);
  }

  static std::unique_ptr<Column> SearchSubset(uint32_t size,
                                              std::vector<uint32_t> index_vec) {
    std::unique_ptr<FakeStorage> storage(
        new FakeStorage(size, SearchStrategy::kBitVector));
    BitVector bv(size);
    for (const uint32_t& i : index_vec) {
      bv.Set(i);
    }
    storage->bit_vector_ = std::move(bv);
    return std::move(storage);
  }

  uint32_t size() const override { return size_; }

 private:
  enum SearchStrategy { kNone, kAll, kRange, kBitVector };
  FakeStorage(uint32_t size, SearchStrategy strategy);

  uint32_t size_ = 0;
  SearchStrategy strategy_ = SearchStrategy::kNone;
  Range range_;
  BitVector bit_vector_;
};

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_FAKE_STORAGE_H_
