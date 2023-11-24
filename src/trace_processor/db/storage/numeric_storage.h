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
#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_NUMERIC_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_NUMERIC_STORAGE_H_

#include <variant>

#include "src/trace_processor/db/storage/storage.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {

namespace protos::pbzero {
class SerializedColumn_Storage;
}

namespace trace_processor {
namespace storage {

// Storage for all numeric type data (i.e. doubles, int32, int64, uint32).
class NumericStorageBase : public Storage {
 public:
  RangeOrBitVector Search(FilterOp op,
                          SqlValue value,
                          RowMap::Range range) const override;

  RangeOrBitVector IndexSearch(FilterOp op,
                               SqlValue value,
                               uint32_t* indices,
                               uint32_t indices_count,
                               bool sorted) const override;

  void StableSort(uint32_t* rows, uint32_t rows_size) const override;

  void Sort(uint32_t* rows, uint32_t rows_size) const override;

  void Serialize(StorageProto*) const override;

  inline uint32_t size() const override { return size_; }

 protected:
  NumericStorageBase(const void* data,
                     uint32_t size,
                     ColumnType type,
                     bool is_sorted = false)
      : size_(size), data_(data), type_(type), is_sorted_(is_sorted) {}

 private:
  BitVector LinearSearchInternal(FilterOp op,
                                 SqlValue val,
                                 RowMap::Range) const;

  BitVector IndexSearchInternal(FilterOp op,
                                SqlValue value,
                                uint32_t* indices,
                                uint32_t indices_count) const;

  RowMap::Range BinarySearchIntrinsic(FilterOp op,
                                      SqlValue val,
                                      RowMap::Range search_range) const;

  RowMap::Range BinarySearchExtrinsic(FilterOp op,
                                      SqlValue val,
                                      uint32_t* indices,
                                      uint32_t indices_count) const;

  const uint32_t size_ = 0;
  const void* data_ = nullptr;
  const ColumnType type_ = ColumnType::kDummy;
  const bool is_sorted_ = false;
};

// Storage for all numeric type data (i.e. doubles, int32, int64, uint32).
template <typename T>
class NumericStorage : public NumericStorageBase {
 public:
  NumericStorage(const std::vector<T>* vec,
                 ColumnType type,
                 bool is_sorted = false)
      : NumericStorageBase(vec->data(),
                           static_cast<uint32_t>(vec->size()),
                           type,
                           is_sorted),
        vector_(vec) {}

 private:
  // TODO(b/307482437): After the migration vectors should be owned by storage,
  // so change from pointer to value.
  const std::vector<T>* vector_;
};

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_NUMERIC_STORAGE_H_
