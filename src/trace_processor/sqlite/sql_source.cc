/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/sqlite/sql_source.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

std::pair<uint32_t, uint32_t> UpdateLineAndColumnForOffset(
    const std::string& sql,
    uint32_t line,
    uint32_t column,
    uint32_t offset) {
  if (offset == 0) {
    return std::make_pair(line, column);
  }

  const char* new_start = sql.c_str() + offset;
  size_t prev_nl = sql.rfind('\n', offset - 1);
  ssize_t nl_count = std::count(sql.c_str(), new_start, '\n');
  PERFETTO_DCHECK((nl_count == 0) == (prev_nl == std::string_view::npos));

  if (prev_nl == std::string::npos) {
    return std::make_pair(line + static_cast<uint32_t>(nl_count),
                          column + static_cast<uint32_t>(offset));
  }

  ssize_t new_column = std::distance(sql.c_str() + prev_nl, new_start);
  return std::make_pair(line + static_cast<uint32_t>(nl_count),
                        static_cast<uint32_t>(new_column));
}

}  // namespace

SqlSource::SqlSource(std::string sql,
                     std::string name,
                     bool include_traceback_header,
                     uint32_t line,
                     uint32_t col)
    : sql_(std::move(sql)),
      name_(std::move(name)),
      include_traceback_header_(include_traceback_header),
      line_(line),
      col_(col) {}

SqlSource SqlSource::FromExecuteQuery(std::string sql) {
  return SqlSource(std::move(sql), "File \"stdin\"", true, 1, 1);
}

SqlSource SqlSource::FromMetric(std::string sql, const std::string& name) {
  return SqlSource(std::move(sql), "Metric \"" + name + "\"", true, 1, 1);
}

SqlSource SqlSource::FromFunction(std::string sql, const std::string& name) {
  return SqlSource(std::move(sql), "Function \"" + name + "\"", false, 1, 1);
}

SqlSource SqlSource::FromMetricFile(std::string sql, const std::string& name) {
  return SqlSource(std::move(sql), "Metric file \"" + name + "\"", false, 1, 1);
}

SqlSource SqlSource::FromModuleImport(std::string sql,
                                      const std::string& module) {
  return SqlSource(std::move(sql), "Module import \"" + module + "\"", false, 1,
                   1);
}

SqlSource SqlSource::FromSpanJoin(std::string sql, const std::string& name) {
  return SqlSource(std::move(sql), "Span Join Table \"" + name + "\"", false, 1,
                   1);
}

SqlSource SqlSource::Substr(uint32_t offset, uint32_t len) const {
  auto line_and_col = UpdateLineAndColumnForOffset(sql_, line_, col_, offset);
  return SqlSource(sql_.substr(offset, len), name_, include_traceback_header_,
                   line_and_col.first, line_and_col.second);
}

std::string SqlSource::AsTracebackFrame(
    std::optional<uint32_t> opt_offset) const {
  uint32_t offset = opt_offset.value_or(0);

  size_t start_idx = offset - std::min<size_t>(128ul, offset);
  if (offset > 0) {
    size_t prev_nl = sql_.rfind('\n', offset - 1);
    if (prev_nl != std::string::npos) {
      start_idx = std::max(prev_nl + 1, start_idx);
    }
  }

  size_t end_idx = std::min<size_t>(offset + 128ul, sql_.size());
  size_t next_nl = sql_.find('\n', offset);
  if (next_nl != std::string::npos) {
    end_idx = std::min(next_nl, end_idx);
  }
  size_t caret_pos = offset - start_idx;

  std::string header;
  if (include_traceback_header_) {
    header = "Traceback (most recent call last):\n";
  }

  auto line_and_col = UpdateLineAndColumnForOffset(sql_, line_, col_, offset);
  std::string sql_segment = sql_.substr(start_idx, end_idx - start_idx);
  std::string caret = std::string(caret_pos, ' ') + "^";
  base::StackString<1024> str("%s  %s line %u col %u\n    %s\n    %s\n",
                              header.c_str(), name_.c_str(), line_and_col.first,
                              line_and_col.second, sql_segment.c_str(),
                              caret.c_str());
  return str.ToStdString();
}

}  // namespace trace_processor
}  // namespace perfetto
