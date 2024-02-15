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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_STRING_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_STRING_STORAGE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

// Storage for String columns.
class StringStorage final : public DataLayer {
 public:
  StringStorage(StringPool* string_pool,
                const std::vector<StringPool::Id>* data,
                bool is_sorted = false);

  std::unique_ptr<DataLayerChain> MakeChain() override;

 private:
  class ChainImpl : public DataLayerChain {
   public:
    ChainImpl(StringPool* string_pool,
              const std::vector<StringPool::Id>* data,
              bool is_sorted);

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
                    SortDirection direction) const override;

    void Serialize(StorageProto*) const override;

    uint32_t size() const override {
      return static_cast<uint32_t>(data_->size());
    }

    std::string DebugString() const override { return "StringStorage"; }

   private:
    BitVector LinearSearch(FilterOp, SqlValue, Range) const;

    RangeOrBitVector IndexSearchInternal(FilterOp op,
                                         SqlValue sql_val,
                                         const uint32_t* indices,
                                         uint32_t indices_size) const;

    Range BinarySearchIntrinsic(FilterOp op,
                                SqlValue val,
                                Range search_range) const;

    // TODO(b/307482437): After the migration vectors should be owned by
    // storage, so change from pointer to value.
    const std::vector<StringPool::Id>* data_ = nullptr;
    StringPool* string_pool_ = nullptr;
    const bool is_sorted_ = false;
  };

  const std::vector<StringPool::Id>* data_ = nullptr;
  StringPool* string_pool_ = nullptr;
  const bool is_sorted_ = false;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_STRING_STORAGE_H_
