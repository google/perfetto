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

#include <cstdio>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor {

// Prints a result set as CSV with every string quoted, NULL as "[NULL]" and
// doubles with six decimal places. Diff tests and scripts depend on this exact
// format: do not change it.
class CsvResultFormatter : public ResultFormatter {
 public:
  CsvResultFormatter(std::vector<std::string> column_names, FILE* output);
  ~CsvResultFormatter() override;

  CsvResultFormatter(const CsvResultFormatter&) = delete;
  CsvResultFormatter& operator=(const CsvResultFormatter&) = delete;

  bool AddRow(const SqlValue* cells) override;
  void Finish() override;

 private:
  const std::vector<std::string> column_names_;
  FILE* const output_;
  std::string rows_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SHELL_CSV_RESULT_FORMATTER_H_
