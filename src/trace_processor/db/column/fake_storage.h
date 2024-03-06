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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

// Fake implementation of Storage for use in tests.
class FakeStorage final : public DataLayer {
 public:
  std::unique_ptr<DataLayerChain> MakeChain() override;

  static std::unique_ptr<DataLayer> SearchAll(uint32_t size) {
    return std::unique_ptr<DataLayer>(
        new FakeStorage(size, SearchStrategy::kAll));
  }

  static std::unique_ptr<DataLayer> SearchNone(uint32_t size) {
    return std::unique_ptr<DataLayer>(
        new FakeStorage(size, SearchStrategy::kNone));
  }

  static std::unique_ptr<DataLayer> SearchSubset(uint32_t size, Range r) {
    std::unique_ptr<FakeStorage> storage(
        new FakeStorage(size, SearchStrategy::kRange));
    storage->range_ = r;
    return std::move(storage);
  }

  static std::unique_ptr<DataLayer> SearchSubset(uint32_t size, BitVector bv) {
    std::unique_ptr<FakeStorage> storage(
        new FakeStorage(size, SearchStrategy::kBitVector));
    storage->bit_vector_ = std::move(bv);
    return std::move(storage);
  }

  static std::unique_ptr<DataLayer> SearchSubset(
      uint32_t size,
      const std::vector<uint32_t>& index_vec) {
    std::unique_ptr<FakeStorage> storage(
        new FakeStorage(size, SearchStrategy::kBitVector));
    BitVector bv(size);
    for (uint32_t i : index_vec) {
      bv.Set(i);
    }
    storage->bit_vector_ = std::move(bv);
    return std::move(storage);
  }

 private:
  enum SearchStrategy { kNone, kAll, kRange, kBitVector };

  class ChainImpl : public DataLayerChain {
   public:
    ChainImpl(uint32_t, SearchStrategy, Range, BitVector);

    SingleSearchResult SingleSearch(FilterOp,
                                    SqlValue,
                                    uint32_t) const override;

    SearchValidationResult ValidateSearchConstraints(FilterOp,
                                                     SqlValue) const override;

    RangeOrBitVector SearchValidated(FilterOp, SqlValue, Range) const override;

    RangeOrBitVector IndexSearchValidated(FilterOp,
                                          SqlValue,
                                          Indices) const override;

    Range OrderedIndexSearchValidated(FilterOp,
                                      SqlValue,
                                      Indices) const override;

    void StableSort(SortToken* start,
                    SortToken* end,
                    SortDirection) const override;

    void Serialize(StorageProto*) const override;

    uint32_t size() const override { return size_; }

    std::string DebugString() const override { return "FakeStorage"; }

   private:
    uint32_t size_ = 0;
    SearchStrategy strategy_ = SearchStrategy::kNone;
    Range range_;
    BitVector bit_vector_;
  };

  FakeStorage(uint32_t size, SearchStrategy strategy);

  uint32_t size_ = 0;
  SearchStrategy strategy_ = SearchStrategy::kNone;
  Range range_;
  BitVector bit_vector_;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_FAKE_STORAGE_H_
