/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_RANGE_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_RANGE_OVERLAY_H_

#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

class RangeOverlay final : public DataLayer {
 public:
  explicit RangeOverlay(const Range*);
  ~RangeOverlay() override;

  std::unique_ptr<DataLayerChain> MakeChain(
      std::unique_ptr<DataLayerChain>,
      ChainCreationArgs = ChainCreationArgs());

 private:
  class ChainImpl : public DataLayerChain {
   public:
    ChainImpl(std::unique_ptr<DataLayerChain>, const Range*);

    SingleSearchResult SingleSearch(FilterOp,
                                    SqlValue,
                                    uint32_t) const override;

    UniqueSearchResult UniqueSearch(FilterOp,
                                    SqlValue,
                                    uint32_t*) const override;

    SearchValidationResult ValidateSearchConstraints(FilterOp,
                                                     SqlValue) const override;

    RangeOrBitVector SearchValidated(FilterOp, SqlValue, Range) const override;

    RangeOrBitVector IndexSearchValidated(FilterOp p,
                                          SqlValue,
                                          Indices) const override;

    Range OrderedIndexSearchValidated(FilterOp,
                                      SqlValue,
                                      Indices) const override;

    void StableSort(SortToken* start,
                    SortToken* end,
                    SortDirection) const override;

    void Serialize(StorageProto*) const override;

    uint32_t size() const override { return range_->size(); }

    std::string DebugString() const override { return "RangeOverlay"; }

   private:
    std::unique_ptr<DataLayerChain> inner_;
    const Range* range_;
  };

  const Range* range_;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_RANGE_OVERLAY_H_
