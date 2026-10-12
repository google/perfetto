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

#include "src/trace_processor/shell/result_formatter.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/trace_processor/shell/csv_result_formatter.h"
#include "src/trace_processor/shell/markdown_result_formatter.h"

namespace perfetto::trace_processor {

ResultFormatter::~ResultFormatter() = default;

uint64_t CutToChars(std::string_view* s, uint64_t max_chars) {
  if (max_chars == 0) {
    return 0;
  }
  // Counts every byte which is not a UTF-8 continuation byte.
  size_t cut = s->size();
  uint64_t chars = 0;
  for (size_t i = 0; i < s->size(); ++i) {
    if ((static_cast<uint8_t>((*s)[i]) & 0xC0) == 0x80) {
      continue;
    }
    if (chars == max_chars) {
      cut = i;
    }
    ++chars;
  }
  *s = s->substr(0, cut);
  return chars > max_chars ? chars - max_chars : 0;
}

std::string CutMarker(uint64_t dropped_chars) {
  return "…(+" + std::to_string(dropped_chars) + " chars)";
}

ResultFormatOptions DefaultMarkdownOptions() {
  ResultFormatOptions options;
  options.format = ResultFormat::kMarkdown;
  options.max_rows = 1000;
  options.max_bytes = 10000;
  options.max_cell_chars = 500;
  options.max_rows_past_limit = 100000;
  return options;
}

std::unique_ptr<ResultFormatter> CreateResultFormatter(
    const ResultFormatOptions& options,
    std::vector<std::string> column_names,
    FILE* output) {
  switch (options.format) {
    case ResultFormat::kCsv:
      return std::make_unique<CsvResultFormatter>(
          options, std::move(column_names), output, stderr);
    case ResultFormat::kMarkdown:
      return std::make_unique<MarkdownResultFormatter>(
          options, std::move(column_names), output);
  }
  return nullptr;
}

}  // namespace perfetto::trace_processor
