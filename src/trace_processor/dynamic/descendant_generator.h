/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DYNAMIC_DESCENDANT_GENERATOR_H_
#define SRC_TRACE_PROCESSOR_DYNAMIC_DESCENDANT_GENERATOR_H_

#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/dynamic/dynamic_table_generator.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Implements the following dynamic tables:
// * descendant_slice
// * descendant_slice_by_stack
//
// See docs/analysis/trace-processor for usage.
class DescendantGenerator : public DynamicTableGenerator {
 public:
  enum class Descendant { kSlice = 1, kSliceByStack = 2 };

  DescendantGenerator(Descendant type, TraceProcessorContext* context);

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

  // Returns a RowMap of slice IDs which are descendants of |slice_id|. Returns
  // NULL if an invalid |slice_id| is given. This is used by
  // ConnectedFlowGenerator to traverse flow indirectly connected flow events.
  static base::Optional<RowMap> GetDescendantSlices(
      const tables::SliceTable& slices,
      SliceId slice_id);

 private:
  Descendant type_;
  TraceProcessorContext* context_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_DESCENDANT_GENERATOR_H_
