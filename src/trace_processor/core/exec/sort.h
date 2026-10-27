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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SORT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SORT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/sorted_rows.h"

namespace perfetto::trace_processor::core::exec {

struct SortSpec {
  using Key = SortKey;
  std::vector<Key> keys;
  // The Uint32 column holding each row's number in the store it is parked in
  // (see ParkRows): set by the planner when it parks the rows.
  uint32_t numbers = 0;
};

// Stably sorts rows parked in a store (see ParkRows) by their keys. Nulls
// sort as in SQLite: first ascending, last descending.
//
// Holds only the keys and each row's number in the store, and outputs one
// Uint32 column: the numbers, in sorted order. Rows already in order come out
// numbered one after another, which FetchRows reads without copying.
// TODO(lalitm): support string keys, which needs the string pool for ranks.
class Sort : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) override {
    out->keeps = StepColumns::Keeps::kRowNumbers;
    for (SortKey& key : spec_.keys) {
      out->reads.push_back(&key.column);
    }
    out->numbers = &spec_.numbers;
  }

  explicit Sort(SortSpec);
  ~Sort() override;

 private:
  friend class PipelineFormatter;

  struct State : Breaker::State {
    ~State() override;
    void Reset() override;

    SortedRows rows;
    uint32_t emitted = 0;
  };

  std::unique_ptr<Breaker::State> CreateState() const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  SortSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SORT_H_
