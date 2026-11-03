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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_EMPTY_SOURCE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_EMPTY_SOURCE_H_

#include <memory>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// Produces no batches: the source of a plan known to return no rows, such as
// one filtering a column by a condition which can hold for none of them.
// Operators after it still finish, so a plan which outputs a row for no input
// still does.
class EmptySource final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  EmptySource();
  ~EmptySource() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool GetData(RowBatch& out, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_EMPTY_SOURCE_H_
