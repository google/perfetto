/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INDEX_SCAN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INDEX_SCAN_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// Reads the rows of a dataframe which an index finds for equalities on its
// leading columns, in index order, as the dataframe interpreter does. Each
// equality narrows the index's entries; any other condition is left to a
// Filter after the scan. The searches run once per run, on the first
// GetData(). The columns must be finalized.
class IndexScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // = on one of the index's columns, given in index order.
  struct Equality {
    std::shared_ptr<const dataframe::Column> column;
    // Index into the run's filter values.
    uint32_t value = 0;
  };

  // `pool` must outlive the scan.
  IndexScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
            const dataframe::Index& index,
            std::vector<Equality> equalities,
            const StringPool* pool);
  ~IndexScan() override;

  std::unique_ptr<OperatorState> MakeState(Context& context) const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    ~State() override;
    RowListReader reader;
    const Context* context = nullptr;
    // By equality: prefix popcounts of sparse null columns.
    std::vector<Slab<uint32_t>> popcounts;
    // By equality: the column as filter::SearchIndexEqual takes it.
    std::vector<filter::IndexColumn> columns;
    bool searched = false;
  };

  Span<const uint32_t> Find(const State& s) const;

  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  std::shared_ptr<std::vector<uint32_t>> permutation_;
  std::vector<Equality> equalities_;
  const StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INDEX_SCAN_H_
