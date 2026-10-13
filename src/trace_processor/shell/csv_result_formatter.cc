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

#include "src/trace_processor/shell/csv_result_formatter.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {

CsvResultFormatter::CsvResultFormatter(const ResultFormatOptions& options,
                                       std::vector<std::string> column_names,
                                       FILE* output,
                                       FILE* notes)
    : options_(options),
      column_names_(std::move(column_names)),
      output_(output),
      notes_(notes) {}

CsvResultFormatter::~CsvResultFormatter() = default;

std::string CsvResultFormatter::FormatRow(const SqlValue* cells) const {
  std::string row;
  for (size_t c = 0; c < column_names_.size(); c++) {
    if (c > 0) {
      row += ',';
    }
    const SqlValue& value = cells[c];
    switch (value.type) {
      case SqlValue::Type::kNull:
        row += "\"[NULL]\"";
        break;
      case SqlValue::Type::kDouble:
        row += base::StackString<256>("%f", value.double_value).ToStdString();
        break;
      case SqlValue::Type::kLong:
        row +=
            base::StackString<256>("%" PRIi64, value.long_value).ToStdString();
        break;
      case SqlValue::Type::kString: {
        std::string_view str = value.string_value;
        uint64_t dropped = CutToChars(&str, options_.max_cell_chars);
        row += '"';
        row += str;
        if (dropped) {
          row += CutMarker(dropped);
        }
        row += '"';
        break;
      }
      case SqlValue::Type::kBytes:
        row += "\"<raw bytes>\"";
        break;
    }
  }
  row += '\n';
  return row;
}

bool CsvResultFormatter::AddRow(const SqlValue* cells) {
  ++row_count_;
  if (!limit_exceeded_) {
    std::string row = FormatRow(cells);
    // The first row is always kept so that wide rows still show something.
    bool over_rows = options_.max_rows && rows_kept_ >= options_.max_rows;
    bool over_bytes = options_.max_bytes && rows_kept_ > 0 &&
                      rows_.size() + row.size() > options_.max_bytes;
    if (!over_rows && !over_bytes) {
      rows_ += row;
      ++rows_kept_;
      return true;
    }
    limit_exceeded_ = true;
  }
  if (options_.max_rows_past_limit &&
      ++rows_past_limit_ >= options_.max_rows_past_limit) {
    stopped_early_ = true;
    return false;
  }
  return true;
}

void CsvResultFormatter::Finish() {
  std::string header;
  for (size_t c = 0; c < column_names_.size(); c++) {
    if (c > 0) {
      header += ',';
    }
    header += '"' + column_names_[c] + '"';
  }
  header += '\n';
  fwrite(header.data(), 1, header.size(), output_);
  fwrite(rows_.data(), 1, rows_.size(), output_);
  if (limit_exceeded_) {
    fprintf(notes_,
            "(first %" PRIu64 " of %s%" PRIu64
            " rows. Pass --max-rows 0 --max-bytes 0 for all)\n",
            rows_kept_, stopped_early_ ? "at least " : "", row_count_);
  }
}

}  // namespace perfetto::trace_processor
