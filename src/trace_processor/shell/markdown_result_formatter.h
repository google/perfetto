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

#ifndef SRC_TRACE_PROCESSOR_SHELL_MARKDOWN_RESULT_FORMATTER_H_
#define SRC_TRACE_PROCESSOR_SHELL_MARKDOWN_RESULT_FORMATTER_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor {

// Prints a result set as a compact markdown table: no alignment padding, NULL
// as `NULL`, `|` and line breaks escaped and long cells cut with a
// `…(+N chars)` marker.
//
// Result sets over the row or byte limits are printed as their first and last
// rows with a `| … N rows omitted … |` marker row in between. A footer then
// states exactly what was omitted and how to get it.
class MarkdownResultFormatter : public ResultFormatter {
 public:
  MarkdownResultFormatter(const ResultFormatOptions& options,
                          std::vector<std::string> column_names,
                          FILE* output);
  ~MarkdownResultFormatter() override;

  MarkdownResultFormatter(const MarkdownResultFormatter&) = delete;
  MarkdownResultFormatter& operator=(const MarkdownResultFormatter&) = delete;

  bool AddRow(const SqlValue* cells) override;
  void Finish() override;

 private:
  std::string FormatRow(const SqlValue* cells) const;
  void OnLimitExceeded();
  std::string Footer(size_t tail_rows) const;

  const ResultFormatOptions options_;
  const std::vector<std::string> column_names_;
  FILE* const output_;
  // Number of rows printed at each end of a result set over the limits.
  const size_t edge_rows_;

  // All rows while the result is within the limits. Once a limit is
  // exceeded, only the first few rows stay here and the most recent ones are
  // kept in |tail_|.
  std::vector<std::string> head_;
  std::deque<std::string> tail_;
  uint64_t head_bytes_ = 0;
  bool limit_exceeded_ = false;
  bool stopped_early_ = false;

  uint64_t row_count_ = 0;
  uint64_t rows_past_limit_ = 0;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SHELL_MARKDOWN_RESULT_FORMATTER_H_
