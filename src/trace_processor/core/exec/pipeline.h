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

#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// A source and a straight line of operators, exposed as a Source. Its state
// holds the states of everything in it.
class PullPipeline : public Source {
 public:
  PullPipeline(const Source&, std::vector<std::unique_ptr<Operator>>);
  ~PullPipeline() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  struct State : OperatorState {
    ~State() override;
    std::unique_ptr<OperatorState> source;
    std::vector<std::unique_ptr<OperatorState>> operators;
  };

  const Source& source_;
  std::vector<std::unique_ptr<Operator>> operators_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_H_
