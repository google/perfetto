/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQL_SOURCE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQL_SOURCE_H_

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace perfetto {
namespace trace_processor {

// An SQL string which retains knowledge of the source of the SQL (i.e. stdlib
// module, ExecuteQuery etc).
class SqlSource {
 public:
  class Rewriter;

  // Creates a SqlSource instance wrapping SQL passed to
  // |TraceProcessor::ExecuteQuery|.
  static SqlSource FromExecuteQuery(std::string sql);

  // Creates a SqlSource instance wrapping SQL executed when running a metric.
  static SqlSource FromMetric(std::string sql, const std::string& metric_file);

  // Creates a SqlSource instance wrapping SQL executed when running a metric
  // file (i.e. with RUN_METRIC).
  static SqlSource FromMetricFile(std::string sql,
                                  const std::string& metric_file);

  // Creates a SqlSource instance wrapping SQL executed when importing a module.
  static SqlSource FromModuleImport(std::string sql, const std::string& module);

  // Creates a SqlSource instance wrapping SQL which is an internal
  // implementation detail of trace processor.
  static SqlSource FromTraceProcessorImplementation(std::string sql);

  // Returns this SqlSource instance as a string which can be appended as a
  // "traceback" frame to an error message. Callers should pass an |offset|
  // parameter which indicates the exact location of the error in the SQL
  // string. 0 and |sql().size()| are both valid offset positions and correspond
  // to the start and end of the source respectively.
  //
  // Specifically, this string will include:
  //  a) context about the source of the SQL
  //  b) line and column number of the error
  //  c) a snippet of the SQL and a caret (^) character pointing to the location
  //     of the error.
  std::string AsTraceback(uint32_t offset) const;

  // Same as |AsTraceback| but for offsets which come from SQLite instead of
  // from trace processor tokenization or parsing.
  std::string AsTracebackForSqliteOffset(std::optional<uint32_t> offset) const;

  // Creates a SqlSource instance with the SQL taken as a substring starting
  // at |offset| with |len| characters.
  //
  // Note: this function should only be called if |this| has not already been
  // rewritten (i.e. it is undefined behaviour if |IsRewritten()| returns true).
  SqlSource Substr(uint32_t offset, uint32_t len) const;

  // Creates a SqlSource instance with the execution SQL rewritten to
  // |rewrite_sql| but preserving the context from |this|.
  //
  // This is useful when PerfettoSQL statements are transpiled into SQLite
  // statements but we want to preserve the context of the original statement.
  //
  // Note: this function should only be called if |this| has not already been
  // rewritten (i.e. it is undefined behaviour if |IsRewritten()| returns true).
  SqlSource FullRewrite(SqlSource) const;

  // Returns the SQL string backing this SqlSource instance;
  const std::string& sql() const { return sql_; }

  // Returns whether this SqlSource has been rewritten.
  bool IsRewritten() const { return !root_.rewrites.empty(); }

 private:
  struct Rewrite;
  // Represents a tree of SQL rewrites, preserving the source for each rewrite.
  struct Node {
    std::string name;
    std::string sql;
    bool include_traceback_header = false;
    uint32_t line = 1;
    uint32_t col = 1;
    std::vector<Rewrite> rewrites;

    std::string AsTraceback(uint32_t offset) const;
    std::string SelfTraceback(uint32_t offset) const;
    Node Substr(uint32_t offset, uint32_t len) const;
  };
  struct Rewrite {
    uint32_t rewritten_start;
    uint32_t rewritten_end;
    uint32_t original_start;
    uint32_t original_end;
    Node node;
  };

  SqlSource() = default;
  SqlSource(std::string sql, std::string name, bool include_traceback_header);

  std::string sql_;
  Node root_;
};

// Used to rewrite a SqlSource using SQL from other SqlSources.
class SqlSource::Rewriter {
 public:
  // Creates a Rewriter object which can be used to rewrite the SQL backing
  // |source|.
  //
  // Note: this function should only be called if |source| has not already been
  // rewritten (i.e. it is undefined behaviour if |source.IsRewritten()| returns
  // true).
  explicit Rewriter(SqlSource source);

  // Replaces the SQL between |start| and |end| with the contents of |rewrite|.
  void Rewrite(uint32_t start, uint32_t end, SqlSource rewrite);

  // Returns the rewritten SqlSource instance.
  SqlSource Build() &&;

 private:
  using BoundedRewrite =
      std::tuple<uint32_t /* start */, uint32_t /* end */, SqlSource>;

  SqlSource orig_;
  std::vector<BoundedRewrite> pending_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQL_SOURCE_H_
