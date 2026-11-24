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

#ifndef SRC_TRACE_PROCESSOR_SHELL_CSV_RESULT_FORMATTER_H_
#define SRC_TRACE_PROCESSOR_SHELL_CSV_RESULT_FORMATTER_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor {

// Prints a result set as CSV with every string quoted, NULL as "[NULL]" and
// doubles with six decimal places. Diff tests and scripts depend on this exact
// format: do not change it.
//
// A result set over the limits in the options is cut to its first rows. The
// note saying so goes to |notes| so that |output| stays valid CSV.
class CsvResultFormatter : public ResultFormatter {
 public:
  CsvResultFormatter(const ResultFormatOptions& options,
                     std::vector<std::string> column_names,
                     FILE* output,
                     FILE* notes);
  ~CsvResultFormatter() override;

  CsvResultFormatter(const CsvResultFormatter&) = delete;
  CsvResultFormatter& operator=(const CsvResultFormatter&) = delete;

  bool AddRow(const SqlValue* cells) override;
  void Finish() override;

 private:
  std::string FormatRow(const SqlValue* cells) const;

  const ResultFormatOptions options_;
  const std::vector<std::string> column_names_;
  FILE* const output_;
  FILE* const notes_;
  std::string rows_;
  uint64_t rows_kept_ = 0;
  uint64_t row_count_ = 0;
  uint64_t rows_past_limit_ = 0;
  bool limit_exceeded_ = false;
  bool stopped_early_ = false;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SHELL_CSV_RESULT_FORMATTER_H_
