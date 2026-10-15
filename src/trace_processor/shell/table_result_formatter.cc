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

#include "src/trace_processor/shell/table_result_formatter.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor {
namespace {

void AppendPadded(std::string_view text,
                  size_t width,
                  bool right_align,
                  std::string* out) {
  std::string padding(width - std::min(width, CountChars(text)), ' ');
  if (right_align) {
    *out += padding;
  }
  *out += text;
  if (!right_align) {
    *out += padding;
  }
}

}  // namespace

bool AskForMoreRows() {
  fprintf(stderr, "... Enter for more rows, q to stop: ");
  fflush(stderr);
  char input[32];
  if (!fgets(input, sizeof(input), stdin)) {
    // Ctrl-D only ends this result: let the shell keep reading stdin.
    clearerr(stdin);
    fprintf(stderr, "\n");
    return false;
  }
  return input[0] != 'q';
}

TableResultFormatter::TableResultFormatter(
    const ResultFormatOptions& options,
    std::vector<std::string> column_names,
    FILE* output,
    std::function<bool()> more)
    : options_(options),
      column_names_(std::move(column_names)),
      output_(output),
      more_(std::move(more)) {}

TableResultFormatter::~TableResultFormatter() = default;

bool TableResultFormatter::AddRow(const SqlValue* cells) {
  // Only ask for another page once there is a row to put on it.
  if (options_.page_rows && page_.size() == options_.page_rows) {
    PrintPage();
    if (!first_prompt_) {
      first_prompt_ = std::chrono::steady_clock::now();
    }
    if (!more_()) {
      stopped_ = true;
      return false;
    }
  }
  ++row_count_;
  Row row;
  for (size_t i = 0; i < column_names_.size(); ++i) {
    row.push_back(FormatCell(cells[i]));
  }
  page_.push_back(std::move(row));
  return true;
}

void TableResultFormatter::Finish() {
  // A result which ended exactly on a page boundary has nothing left.
  if (!page_.empty() || !first_prompt_) {
    PrintPage();
  }
  if (stopped_) {
    fprintf(output_, "(stopped after %" PRIu64 " rows)\n", row_count_);
  } else if (row_count_ == 0 || row_count_ >= 10) {
    fprintf(output_, "(%" PRIu64 " rows)\n", row_count_);
  }
  fflush(output_);
}

std::optional<std::chrono::steady_clock::time_point>
TableResultFormatter::FirstPromptTime() const {
  return first_prompt_;
}

TableResultFormatter::Cell TableResultFormatter::FormatCell(
    const SqlValue& value) const {
  Cell cell{FormatCellForDisplay(value, options_.max_cell_chars, ""),
            Cell::Kind::kText};
  if (value.type == SqlValue::Type::kNull) {
    cell.kind = Cell::Kind::kNull;
  } else if (value.type == SqlValue::Type::kLong ||
             value.type == SqlValue::Type::kDouble) {
    cell.kind = Cell::Kind::kNumber;
  }
  return cell;
}

void TableResultFormatter::PrintPage() {
  size_t columns = column_names_.size();
  std::vector<size_t> widths(columns);
  std::vector<bool> has_number(columns);
  std::vector<bool> has_text(columns);
  for (size_t i = 0; i < columns; ++i) {
    widths[i] = CountChars(column_names_[i]);
  }
  for (const auto& row : page_) {
    for (size_t i = 0; i < columns; ++i) {
      widths[i] = std::max(widths[i], CountChars(row[i].text));
      has_number[i] = has_number[i] || row[i].kind == Cell::Kind::kNumber;
      has_text[i] = has_text[i] || row[i].kind == Cell::Kind::kText;
    }
  }

  std::string out;
  auto append_line = [&](const std::vector<std::string_view>& texts) {
    std::string line;
    for (size_t i = 0; i < columns; ++i) {
      if (i > 0) {
        line += "  ";
      }
      AppendPadded(texts[i], widths[i], has_number[i] && !has_text[i], &line);
    }
    line.erase(line.find_last_not_of(' ') + 1);
    out += line + '\n';
  };
  std::vector<std::string> dividers;
  for (size_t width : widths) {
    dividers.emplace_back(width, '-');
  }
  append_line({column_names_.begin(), column_names_.end()});
  append_line({dividers.begin(), dividers.end()});
  for (const auto& row : page_) {
    std::vector<std::string_view> texts;
    for (const auto& cell : row) {
      texts.push_back(cell.text);
    }
    append_line(texts);
  }
  fwrite(out.data(), 1, out.size(), output_);
  fflush(output_);
  page_.clear();
}

}  // namespace perfetto::trace_processor
