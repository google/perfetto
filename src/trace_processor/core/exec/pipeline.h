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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_

#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include "src/trace_processor/core/exec/batch_buffer.h"

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Optional batching policy and execution demand. Finite demand disables
// optional lookahead at every boundary; intrinsic blocking operators still
// consume the input needed to produce their first result. Cancellation is
// checked between source/operator calls, not within individual kernels.
struct ExecutionOptions {
  BatchPreference preference = BatchPreference::kLatency;
  uint64_t limit = std::numeric_limits<uint64_t>::max();
  uint32_t small_batch_rows = 64;
  uint32_t target_batch_rows = kMaxBatchRows;
  std::function<bool()> cancelled;
};

// Pulls a source through a chain of operators. Each boundary retains input
// across continuations and can combine small batches for a throughput consumer.
// Finish runs only after successful exhaustion.
class Pipeline : public Source {
 public:
  Pipeline(const Source&,
           std::vector<std::unique_ptr<Operator>>,
           ExecutionOptions = {});
  Pipeline(Source&&, std::vector<std::unique_ptr<Operator>>) = delete;
  ~Pipeline() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  struct State : OperatorState {
    ~State() override;
    std::unique_ptr<OperatorState> source;
    std::vector<std::unique_ptr<OperatorState>> operators;
    struct Stage {
      RowBatch input;
      RowBatch deferred;
      RowBatch scratch;
      BatchBuffer buffered;
      bool continuation = false;
      bool done = false;
    };
    // One input boundary per operator, plus the output boundary.
    std::vector<Stage> stages;
    uint64_t emitted = 0;
    base::Status status = base::OkStatus();
    bool stopped = false;
    bool source_done = false;
  };

  bool Pull(uint32_t stage, RowBatch&, State&) const;
  bool Input(uint32_t boundary, RowBatch&, State&, BatchPreference) const;
  bool Check(State&) const;
  void Stop(State&) const;

  ExecutionOptions options_;
  const Source& source_;
  std::vector<std::unique_ptr<Operator>> operators_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
