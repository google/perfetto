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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_NUMERIC_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_NUMERIC_STORAGE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

// Storage for all numeric type data (i.e. doubles, int32, int64, uint32).
class NumericStorageBase : public DataLayer {
 protected:
  class ChainImpl : public DataLayerChain {
   public:
    SearchValidationResult ValidateSearchConstraints(FilterOp,
                                                     SqlValue) const override;

    RangeOrBitVector SearchValidated(FilterOp, SqlValue, Range) const override;

    RangeOrBitVector IndexSearchValidated(FilterOp,
                                          SqlValue,
                                          Indices) const override;

    Range OrderedIndexSearchValidated(FilterOp,
                                      SqlValue,
                                      Indices) const override;

    void StableSort(uint32_t*, uint32_t) const override;

    void Sort(uint32_t*, uint32_t) const override;

    void Serialize(StorageProto*) const override;

    std::string DebugString() const override { return "NumericStorage"; }

   protected:
    ChainImpl(const void* vector_ptr, ColumnType type, bool is_sorted);

   private:
    // All viable numeric values for ColumnTypes.
    using NumericValue = std::variant<uint32_t, int32_t, int64_t, double>;

    BitVector LinearSearchInternal(FilterOp op, NumericValue val, Range) const;

    BitVector IndexSearchInternal(FilterOp op,
                                  NumericValue value,
                                  const uint32_t* indices,
                                  uint32_t indices_count) const;

    Range BinarySearchIntrinsic(FilterOp op,
                                NumericValue val,
                                Range search_range) const;

    const void* vector_ptr_ = nullptr;
    const ColumnType storage_type_ = ColumnType::kDummy;
    const bool is_sorted_ = false;
  };

  NumericStorageBase(ColumnType type, bool is_sorted);
  ~NumericStorageBase() override;

  const ColumnType storage_type_ = ColumnType::kDummy;
  const bool is_sorted_ = false;
};

// Storage for all numeric type data (i.e. doubles, int32, int64, uint32).
template <typename T>
class NumericStorage final : public NumericStorageBase {
 public:
  NumericStorage(const std::vector<T>* vec,
                 ColumnType type,
                 bool is_sorted = false)
      : NumericStorageBase(type, is_sorted), vector_(vec) {}

  std::unique_ptr<DataLayerChain> MakeChain() override {
    return std::make_unique<ChainImpl>(vector_, storage_type_, is_sorted_);
  }

 private:
  class ChainImpl : public NumericStorageBase::ChainImpl {
   public:
    ChainImpl(const std::vector<T>* vector, ColumnType type, bool is_sorted)
        : NumericStorageBase::ChainImpl(vector, type, is_sorted),
          vector_(vector) {}

    uint32_t size() const override {
      return static_cast<uint32_t>(vector_->size());
    }

   private:
    const std::vector<T>* vector_;
  };

  const std::vector<T>* vector_;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_NUMERIC_STORAGE_H_
