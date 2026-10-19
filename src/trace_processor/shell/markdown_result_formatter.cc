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

#include "src/trace_processor/shell/markdown_result_formatter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor {
namespace {

// Number of rows printed at each end of a result set over the limits (fewer
// if --max-rows is small).
constexpr size_t kMaxEdgeRows = 20;

// Escaped in cells and headers on top of AppendEscaped()'s defaults, as it
// would end the cell.
constexpr char kMarkdownEscapes[] = "|";

}  // namespace

MarkdownResultFormatter::MarkdownResultFormatter(
    const ResultFormatOptions& options,
    std::vector<std::string> column_names,
    FILE* output)
    : options_(options),
      column_names_(std::move(column_names)),
      output_(output),
      edge_rows_(options.max_rows ? std::max<size_t>(1,
                                                     std::min<uint64_t>(
                                                         kMaxEdgeRows,
                                                         options.max_rows / 2))
                                  : kMaxEdgeRows) {}

MarkdownResultFormatter::~MarkdownResultFormatter() = default;

std::string MarkdownResultFormatter::FormatRow(const SqlValue* cells) const {
  std::string row = "|";
  for (size_t i = 0; i < column_names_.size(); ++i) {
    row += ' ';
    row += FormatCellForDisplay(cells[i], options_.max_cell_chars,
                                kMarkdownEscapes);
    row += " |";
  }
  return row;
}

bool MarkdownResultFormatter::AddRow(const SqlValue* cells) {
  ++row_count_;
  if (!limit_exceeded_) {
    head_.push_back(FormatRow(cells));
    head_bytes_ += head_.back().size() + 1;
    if ((options_.max_rows && head_.size() > options_.max_rows) ||
        (options_.max_bytes && head_bytes_ > options_.max_bytes)) {
      OnLimitExceeded();
    }
    return true;
  }
  tail_.push_back(FormatRow(cells));
  if (tail_.size() > edge_rows_) {
    tail_.pop_front();
  }
  if (options_.max_rows_past_limit &&
      ++rows_past_limit_ >= options_.max_rows_past_limit) {
    stopped_early_ = true;
    return false;
  }
  return true;
}

void MarkdownResultFormatter::OnLimitExceeded() {
  limit_exceeded_ = true;
  // Keep at most |edge_rows_| rows at the start, within half of the byte budget
  // (but always at least one row) so that wide rows can't blow through it.
  uint64_t budget = options_.max_bytes ? options_.max_bytes / 2
                                       : std::numeric_limits<uint64_t>::max();
  uint64_t used = 0;
  size_t keep = 0;
  for (; keep < head_.size() && keep < edge_rows_; ++keep) {
    uint64_t size = head_[keep].size() + 1;
    if (keep > 0 && used + size > budget) {
      break;
    }
    used += size;
  }
  for (size_t i = keep; i < head_.size(); ++i) {
    tail_.push_back(std::move(head_[i]));
    if (tail_.size() > edge_rows_) {
      tail_.pop_front();
    }
  }
  head_.resize(keep);
}

void MarkdownResultFormatter::Finish() {
  std::string out = "|";
  for (const auto& name : column_names_) {
    out += ' ';
    AppendEscaped(name, kMarkdownEscapes, &out);
    out += " |";
  }
  out += "\n|";
  for (size_t i = 0; i < column_names_.size(); ++i) {
    out += "---|";
  }
  out += '\n';
  for (const auto& row : head_) {
    out += row;
    out += '\n';
  }

  // The last rows are only meaningful if the whole result was read. Pick as
  // many as fit in the other half of the byte budget.
  size_t tail_rows = 0;
  if (limit_exceeded_ && !stopped_early_) {
    uint64_t budget = options_.max_bytes ? options_.max_bytes / 2
                                         : std::numeric_limits<uint64_t>::max();
    uint64_t used = 0;
    for (auto it = tail_.rbegin(); it != tail_.rend(); ++it, ++tail_rows) {
      uint64_t size = it->size() + 1;
      if (tail_rows > 0 && used + size > budget) {
        break;
      }
      used += size;
    }
  }
  if (stopped_early_) {
    out += "| … |\n";
  } else if (limit_exceeded_) {
    uint64_t omitted = row_count_ - head_.size() - tail_rows;
    out += "| … " + std::to_string(omitted) + " rows omitted … |\n";
    for (size_t i = tail_.size() - tail_rows; i < tail_.size(); ++i) {
      out += tail_[i];
      out += '\n';
    }
  }

  // A blank line ends the table: a markdown table would otherwise absorb the
  // footer as one more row.
  std::string footer = Footer(tail_rows);
  if (!footer.empty()) {
    out += "\n(" + footer + ")\n";
  }
  fwrite(out.data(), 1, out.size(), output_);
}

std::string MarkdownResultFormatter::Footer(size_t tail_rows) const {
  std::string rows = std::to_string(row_count_);
  if (stopped_early_) {
    return "first " + std::to_string(head_.size()) + " of at least " + rows +
           " rows; stopped reading early. Narrow the query with WHERE, "
           "GROUP BY or LIMIT, or pass --max-rows 0 --max-bytes 0 for all";
  }
  if (limit_exceeded_) {
    return "first " + std::to_string(head_.size()) + " and last " +
           std::to_string(tail_rows) + " of " + rows +
           " rows. Page with LIMIT/OFFSET, or pass --max-rows 0 --max-bytes 0 "
           "for all";
  }
  if (row_count_ == 0) {
    return "0 rows";
  }
  // Short results are easy to count: only add noise to longer ones.
  if (row_count_ >= 10) {
    return rows + " rows";
  }
  return "";
}

}  // namespace perfetto::trace_processor
