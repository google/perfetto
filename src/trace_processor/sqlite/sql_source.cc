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

#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/sys_types.h"

namespace perfetto {
namespace trace_processor {

namespace {

std::pair<uint32_t, uint32_t> GetLineAndColumnForOffset(const std::string& sql,
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
                     bool include_traceback_header)
    : sql_(sql) {
  root_.name = std::move(name);
  root_.sql = std::move(sql);
  root_.include_traceback_header = include_traceback_header;
}

SqlSource SqlSource::FromExecuteQuery(std::string sql) {
  return SqlSource(std::move(sql), "File \"stdin\"", true);
}

SqlSource SqlSource::FromMetric(std::string sql, const std::string& name) {
  return SqlSource(std::move(sql), "Metric \"" + name + "\"", true);
}

SqlSource SqlSource::FromMetricFile(std::string sql, const std::string& name) {
  return SqlSource(std::move(sql), "Metric file \"" + name + "\"", false);
}

SqlSource SqlSource::FromModuleImport(std::string sql,
                                      const std::string& module) {
  return SqlSource(std::move(sql), "Module import \"" + module + "\"", false);
}

SqlSource SqlSource::FromTraceProcessorImplementation(std::string sql) {
  return SqlSource(std::move(sql), "Trace Processor Internal", false);
}

std::string SqlSource::AsTraceback(std::optional<uint32_t> opt_offset) const {
  uint32_t offset = opt_offset.value_or(0);
  // Unfortunately, there is a bug in pre-3.41.2 versions of SQLite where
  // sqlite3_error_offset can return an offset out of bounds. In these
  // situations, zero the offset.
#if SQLITE_VERSION_NUMBER < 3041002
  if (offset >= sql_.size()) {
    offset = 0;
  }
#else
  PERFETTO_CHECK(offset < sql_.size());
#endif
  return root_.AsTraceback(offset);
}

SqlSource SqlSource::Substr(uint32_t offset, uint32_t len) const {
  PERFETTO_CHECK(!IsRewritten());
  SqlSource source;
  source.sql_ = sql_.substr(offset, len);
  source.root_ = root_.Substr(offset, len);
  return source;
}

SqlSource SqlSource::FullRewrite(SqlSource source) const {
  SqlSource::Rewriter rewriter(*this);
  rewriter.Rewrite(0, static_cast<uint32_t>(sql_.size()), source);
  return std::move(rewriter).Build();
}

SqlSource::Rewriter::Rewriter(SqlSource source) : orig_(std::move(source)) {
  PERFETTO_CHECK(!orig_.IsRewritten());
}

void SqlSource::Rewriter::Rewrite(uint32_t start,
                                  uint32_t end,
                                  SqlSource source) {
  PERFETTO_CHECK(start < end);
  pending_.push_back(std::make_tuple(start, end, std::move(source)));
}

SqlSource SqlSource::Rewriter::Build() && {
  std::string sql;
  const char* ptr = orig_.sql_.data();
  uint32_t prev_idx = 0;
  for (auto& [start, end, source] : pending_) {
    PERFETTO_CHECK(prev_idx <= start);
    sql.append(ptr + prev_idx, ptr + start);

    uint32_t rewrite_start = static_cast<uint32_t>(sql.size());
    uint32_t rewrite_end =
        static_cast<uint32_t>(rewrite_start + source.sql_.size());
    sql.append(source.sql());
    orig_.root_.rewrites.push_back(SqlSource::Rewrite{
        rewrite_start, rewrite_end, start, end, std::move(source.root_)});
    prev_idx = end;
  }
  sql.append(ptr + prev_idx, ptr + orig_.sql_.size());
  orig_.sql_ = std::move(sql);
  return orig_;
}

std::string SqlSource::Node::AsTraceback(uint32_t offset) const {
  uint32_t rewritten_skipped = 0;
  uint32_t original_skipped = 0;
  for (const auto& rewrite : rewrites) {
    if (offset >= rewrite.rewritten_end) {
      original_skipped += rewrite.original_end - rewrite.original_start;
      rewritten_skipped += rewrite.rewritten_end - rewrite.rewritten_start;
      continue;
    }
    if (rewrite.rewritten_start > offset) {
      break;
    }
    std::string res = SelfTraceback(rewrite.rewritten_start -
                                    rewritten_skipped + original_skipped);
    res.append(rewrite.node.AsTraceback(offset - rewrite.rewritten_start));
    return res;
  }
  return SelfTraceback(offset - rewritten_skipped + original_skipped);
}

std::string SqlSource::Node::SelfTraceback(uint32_t offset) const {
  size_t start_idx = offset - std::min<size_t>(128ul, offset);
  if (offset > 0) {
    size_t prev_nl = sql.rfind('\n', offset - 1);
    if (prev_nl != std::string::npos) {
      start_idx = std::max(prev_nl + 1, start_idx);
    }
  }

  size_t end_idx = std::min<size_t>(offset + 128ul, sql.size());
  size_t next_nl = sql.find('\n', offset);
  if (next_nl != std::string::npos) {
    end_idx = std::min(next_nl, end_idx);
  }
  size_t caret_pos = offset - start_idx;

  std::string header;
  if (include_traceback_header) {
    header = "Traceback (most recent call last):\n";
  }

  auto line_and_col = GetLineAndColumnForOffset(sql, line, col, offset);
  std::string sql_segment = sql.substr(start_idx, end_idx - start_idx);
  std::string caret = std::string(caret_pos, ' ') + "^";
  base::StackString<1024> str("%s  %s line %u col %u\n    %s\n    %s\n",
                              header.c_str(), name.c_str(), line_and_col.first,
                              line_and_col.second, sql_segment.c_str(),
                              caret.c_str());
  return str.ToStdString();
}

SqlSource::Node SqlSource::Node::Substr(uint32_t offset, uint32_t len) const {
  PERFETTO_CHECK(rewrites.empty());
  auto line_and_col = GetLineAndColumnForOffset(sql, line, col, offset);
  return Node{name,
              sql.substr(offset, len),
              include_traceback_header,
              line_and_col.first,
              line_and_col.second,
              {}};
}

}  // namespace trace_processor
}  // namespace perfetto
