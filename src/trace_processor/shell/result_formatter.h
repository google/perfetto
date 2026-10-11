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

#ifndef SRC_TRACE_PROCESSOR_SHELL_RESULT_FORMATTER_H_
#define SRC_TRACE_PROCESSOR_SHELL_RESULT_FORMATTER_H_

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {

enum class ResultFormat {
  // Quoted CSV: the historical, machine-parseable format.
  kCsv,
};

struct ResultFormatOptions {
  ResultFormat format = ResultFormat::kCsv;
};

// Prints a single result set. Rows are passed to AddRow() while the statement
// is iterated and the result is printed by Finish(), so that time spent
// printing is not attributed to query execution.
class ResultFormatter {
 public:
  virtual ~ResultFormatter();

  // Adds a row with one cell per column. Returns false once enough rows have
  // been seen that the caller should stop iterating the statement.
  virtual bool AddRow(const SqlValue* cells) = 0;

  // Prints the result set. Called exactly once, after the last row.
  virtual void Finish() = 0;
};

std::unique_ptr<ResultFormatter> CreateResultFormatter(
    const ResultFormatOptions& options,
    std::vector<std::string> column_names,
    FILE* output);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SHELL_RESULT_FORMATTER_H_
