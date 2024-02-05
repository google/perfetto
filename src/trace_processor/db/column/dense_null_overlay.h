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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_DENSE_NULL_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_DENSE_NULL_OVERLAY_H_

#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_node.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

// Overlay which introduces the layer of nullability but without changing the
// "spacing" of the underlying storage i.e. this overlay simply "masks" out
// rows in the underlying storage with nulls.
class DenseNullOverlay : public DataNode {
 public:
  explicit DenseNullOverlay(const BitVector* non_null);

  std::unique_ptr<Queryable> MakeQueryable(std::unique_ptr<Queryable>) override;

 private:
  class Queryable : public DataNode::Queryable {
   public:
    Queryable(std::unique_ptr<DataNode::Queryable> inner,
              const BitVector* non_null);

    SearchValidationResult ValidateSearchConstraints(SqlValue,
                                                     FilterOp) const override;

    RangeOrBitVector Search(FilterOp, SqlValue, Range) const override;

    RangeOrBitVector IndexSearch(FilterOp, SqlValue, Indices) const override;

    Range OrderedIndexSearch(FilterOp, SqlValue, Indices) const override;

    void StableSort(uint32_t* rows, uint32_t rows_size) const override;

    void Sort(uint32_t* rows, uint32_t rows_size) const override;

    void Serialize(StorageProto*) const override;

    uint32_t size() const override { return non_null_->size(); }

    std::string DebugString() const override { return "DenseNullOverlay"; }

   private:
    std::unique_ptr<DataNode::Queryable> inner_;
    const BitVector* non_null_ = nullptr;
  };

  const BitVector* non_null_ = nullptr;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_DENSE_NULL_OVERLAY_H_
