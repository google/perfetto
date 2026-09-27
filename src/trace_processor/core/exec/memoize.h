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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_MEMOIZE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_MEMOIZE_H_

#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Passes on the batches of another source, keeping them as it goes. Once a
// read has reached the end, every read after replays what was kept rather
// than reading the source again.
//
// A read stopped part way keeps nothing, and neither does one whose batches
// cannot be kept: the next read goes back to the source.
class Memoize : public Source {
 public:
  // `source` must outlive this.
  explicit Memoize(const Source& source);
  Memoize(Source&&) = delete;
  ~Memoize() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  const Source& source_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_MEMOIZE_H_
