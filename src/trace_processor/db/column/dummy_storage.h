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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_DUMMY_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_DUMMY_STORAGE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

// Dummy storage. Used for columns that are not supposed to have operations done
// on them.
class DummyStorage final : public DataLayer {
 public:
  class ChainImpl : public DataLayerChain {
   public:
    ChainImpl() = default;

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

    uint32_t size() const override;

    std::string DebugString() const override { return "DummyStorage"; }
  };
  std::unique_ptr<DataLayerChain> MakeChain() override;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_DUMMY_STORAGE_H_
