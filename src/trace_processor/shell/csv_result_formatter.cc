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
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {

CsvResultFormatter::CsvResultFormatter(std::vector<std::string> column_names,
                                       FILE* output)
    : column_names_(std::move(column_names)), output_(output) {}

CsvResultFormatter::~CsvResultFormatter() = default;

bool CsvResultFormatter::AddRow(const SqlValue* cells) {
  for (size_t c = 0; c < column_names_.size(); c++) {
    if (c > 0) {
      rows_ += ',';
    }
    const SqlValue& value = cells[c];
    switch (value.type) {
      case SqlValue::Type::kNull:
        rows_ += "\"[NULL]\"";
        break;
      case SqlValue::Type::kDouble:
        rows_ += base::StackString<256>("%f", value.double_value).ToStdString();
        break;
      case SqlValue::Type::kLong:
        rows_ +=
            base::StackString<256>("%" PRIi64, value.long_value).ToStdString();
        break;
      case SqlValue::Type::kString:
        rows_ += '"';
        rows_ += value.string_value;
        rows_ += '"';
        break;
      case SqlValue::Type::kBytes:
        rows_ += "\"<raw bytes>\"";
        break;
    }
  }
  rows_ += '\n';
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
}

}  // namespace perfetto::trace_processor
