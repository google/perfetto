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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_NUMBER_GROUPS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_NUMBER_GROUPS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// Appends a Uint32 column with the group number of each row, grouping by
// `key_columns`. Groups are numbered from 0 in the order they arrive.
//
// The rows must already be grouped. A new group starts wherever a row's keys
// differ from the previous row, so nothing is buffered. Keys are compared as
// in GroupTable.
class NumberGroups final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit NumberGroups(std::vector<uint32_t> key_columns);
  ~NumberGroups() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    explicit State(Context& c) : OperatorState(ResetEachRun{}), context(&c) {}
    ~State() override;
    void Reset() override;

    // The number of groups so far.
    uint32_t groups = 0;

    // The last row's key values, key kinds and null bits.
    std::vector<int64_t> last_words;
    std::vector<KeyKind> kinds;
    uint64_t last_nulls = 0;

    // Scratch for the current batch: the key values of each column, the null
    // bits of each row, and whether each row starts a group.
    std::vector<FlexVector<int64_t>> words;
    FlexVector<uint64_t> nulls;
    FlexVector<uint8_t> starts;

    // Provides the buffers the group numbers are written to.
    Context* context;
    base::Status status = base::OkStatus();
  };

  std::vector<uint32_t> key_columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_NUMBER_GROUPS_H_
