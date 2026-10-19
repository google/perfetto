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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_FORMATTER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_FORMATTER_H_

#include <string>

#include "src/trace_processor/core/exec/pipeline.h"

namespace perfetto::trace_processor::core::exec {

// Prints pipelines for tests. It is a friend of each operator so it can read
// their arguments directly, which keeps printing code out of the operators.
class PipelineFormatter {
 public:
  // Prints `pipeline` on one line: its input, then each step, e.g.
  // "DataframeScan | Sort(#2, #0 desc) | HashAggregate(keys: #2; COUNT(*))".
  // Columns are shown as batch positions.
  static std::string Format(const Pipeline& pipeline);

 private:
  static std::string SourceString(const Source&);
  static std::string TransformString(const Transform&);
  static std::string OperatorString(const Operator&);
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PIPELINE_FORMATTER_H_
