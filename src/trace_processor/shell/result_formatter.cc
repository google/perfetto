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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/csv_result_formatter.h"
#include "src/trace_processor/shell/markdown_result_formatter.h"
#include "src/trace_processor/shell/table_result_formatter.h"

namespace perfetto::trace_processor {

ResultFormatter::~ResultFormatter() = default;

std::optional<std::chrono::steady_clock::time_point>
ResultFormatter::FirstPromptTime() const {
  return std::nullopt;
}

namespace {

bool IsUtf8Continuation(char c) {
  return (static_cast<uint8_t>(c) & 0xC0) == 0x80;
}

// The shortest representation which parses back to the same double, always
// with a '.' or an exponent so that it reads as a floating point value.
std::string FormatDouble(double d) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.15g", d);
  if (strtod(buf, nullptr) != d) {
    snprintf(buf, sizeof(buf), "%.17g", d);
  }
  std::string res(buf);
  if (res.find_first_of(".eni") == std::string::npos) {
    res += ".0";
  }
  return res;
}

}  // namespace

size_t CountChars(std::string_view s) {
  size_t chars = 0;
  for (char c : s) {
    chars += !IsUtf8Continuation(c);
  }
  return chars;
}

uint64_t CutToChars(std::string_view* s, uint64_t max_chars) {
  if (max_chars == 0) {
    return 0;
  }
  size_t cut = s->size();
  uint64_t chars = 0;
  for (size_t i = 0; i < s->size(); ++i) {
    if (IsUtf8Continuation((*s)[i])) {
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

void AppendEscaped(std::string_view s,
                   std::string_view extra_escapes,
                   std::string* out) {
  for (char c : s) {
    switch (c) {
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (extra_escapes.find(c) != std::string_view::npos) {
          *out += '\\';
        }
        *out += c;
    }
  }
}

std::string FormatCellForDisplay(const SqlValue& value,
                                 uint64_t max_chars,
                                 std::string_view extra_escapes) {
  switch (value.type) {
    case SqlValue::Type::kNull:
      return "NULL";
    case SqlValue::Type::kLong:
      return std::to_string(value.long_value);
    case SqlValue::Type::kDouble:
      return FormatDouble(value.double_value);
    case SqlValue::Type::kString: {
      std::string_view s = value.string_value;
      uint64_t dropped = CutToChars(&s, max_chars);
      std::string out;
      AppendEscaped(s, extra_escapes, &out);
      if (dropped) {
        out += CutMarker(dropped);
      }
      return out;
    }
    case SqlValue::Type::kBytes:
      return "<" + std::to_string(value.bytes_count) + " bytes>";
  }
  PERFETTO_FATAL("For GCC");
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

ResultFormatOptions DefaultTableOptions(uint64_t page_rows) {
  ResultFormatOptions options;
  options.format = ResultFormat::kTable;
  options.max_cell_chars = 80;
  options.page_rows = page_rows;
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
    case ResultFormat::kTable:
      return std::make_unique<TableResultFormatter>(
          options, std::move(column_names), output, AskForMoreRows);
  }
  return nullptr;
}

}  // namespace perfetto::trace_processor
