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

#ifndef SRC_TRACE_PROCESSOR_SHELL_TABLE_RESULT_FORMATTER_H_
#define SRC_TRACE_PROCESSOR_SHELL_TABLE_RESULT_FORMATTER_H_

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor {

// Asks on stderr whether to print more rows. False on 'q' or end of input.
bool AskForMoreRows();

// Prints a result set as an aligned plain-text table, with columns sized to
// their contents. Columns holding only numbers (and NULLs) are right-aligned.
//
// Rows are printed a page of |options.page_rows| at a time (all at once if 0),
// each page with its own header and column widths. Before printing a row past
// a full page, |more| is asked whether to continue.
class TableResultFormatter : public ResultFormatter {
 public:
  TableResultFormatter(const ResultFormatOptions& options,
                       std::vector<std::string> column_names,
                       FILE* output,
                       std::function<bool()> more);
  ~TableResultFormatter() override;

  TableResultFormatter(const TableResultFormatter&) = delete;
  TableResultFormatter& operator=(const TableResultFormatter&) = delete;

  bool AddRow(const SqlValue* cells) override;
  void Finish() override;
  std::optional<std::chrono::steady_clock::time_point> FirstPromptTime()
      const override;

 private:
  struct Cell {
    enum class Kind { kText, kNumber, kNull };
    std::string text;
    Kind kind = Kind::kText;
  };
  using Row = std::vector<Cell>;

  Cell FormatCell(const SqlValue& value) const;
  void PrintPage();

  const ResultFormatOptions options_;
  const std::vector<std::string> column_names_;
  FILE* const output_;
  const std::function<bool()> more_;

  // Rows not yet printed.
  std::vector<Row> page_;
  uint64_t row_count_ = 0;
  bool stopped_ = false;
  std::optional<std::chrono::steady_clock::time_point> first_prompt_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SHELL_TABLE_RESULT_FORMATTER_H_
