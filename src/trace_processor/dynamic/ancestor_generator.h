/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DYNAMIC_ANCESTOR_GENERATOR_H_
#define SRC_TRACE_PROCESSOR_DYNAMIC_ANCESTOR_GENERATOR_H_

#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/dynamic/dynamic_table_generator.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_ANCESTOR_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(AncestorSliceTable, "ancestor_slice")                  \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                      \
  C(tables::SliceTable::Id, start_id, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_ANCESTOR_SLICE_TABLE_DEF);

#define PERFETTO_TP_ANCESTOR_STACK_PROFILE_CALLSITE_TABLE_DEF(NAME, PARENT, C) \
  NAME(AncestorStackProfileCallsiteTable,                                      \
       "experimental_ancestor_stack_profile_callsite")                         \
  PARENT(PERFETTO_TP_STACK_PROFILE_CALLSITE_DEF, C)                            \
  C(tables::StackProfileCallsiteTable::Id, start_id, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_ANCESTOR_STACK_PROFILE_CALLSITE_TABLE_DEF);

#define PERFETTO_TP_ANCESTOR_SLICE_BY_STACK_TABLE_DEF(NAME, PARENT, C) \
  NAME(AncestorSliceByStackTable, "ancestor_slice_by_stack")           \
  PARENT(PERFETTO_TP_SLICE_TABLE_DEF, C)                               \
  C(int64_t, start_stack_id, Column::Flag::kHidden)

PERFETTO_TP_TABLE(PERFETTO_TP_ANCESTOR_SLICE_BY_STACK_TABLE_DEF);

}  // namespace tables

class TraceProcessorContext;

// Implements the following dynamic tables:
// * ancestor_slice
// * experimental_ancestor_stack_profile_callsite
// * ancestor_slice_by_stack
//
// See docs/analysis/trace-processor for usage.
class AncestorGenerator : public DynamicTableGenerator {
 public:
  enum class Ancestor {
    kSlice = 1,
    kStackProfileCallsite = 2,
    kSliceByStack = 3
  };

  AncestorGenerator(Ancestor type, const TraceStorage* storage);

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

  // Returns a vector of rows numbers which are ancestors of |slice_id|.
  // Returns base::nullopt if an invalid |slice_id| is given. This is used by
  // ConnectedFlowGenerator to traverse flow indirectly connected flow events.
  static base::Optional<std::vector<tables::SliceTable::RowNumber>>
  GetAncestorSlices(const tables::SliceTable& slices, SliceId slice_id);

 private:
  Ancestor type_;
  const TraceStorage* storage_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_ANCESTOR_GENERATOR_H_
