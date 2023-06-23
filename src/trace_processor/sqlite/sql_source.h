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

namespace perfetto {
namespace trace_processor {

// An SQL string which retains knowledge of the source of the SQL (i.e. stdlib
// module, ExecuteQuery etc).
//
// The reason this class exists is to allow much better error messages as we
// can not only render of the snippet of SQL which is failing but also point
// to the correct line number in the context of the whole SQL file.
class SqlSource {
 public:
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

  // Creates a SqlSource instance wrapping SQL executed when running a function.
  static SqlSource FromFunction(std::string sql, const std::string& function);

  // Creates a SqlSource instance wrapping SQL executed when executing a SPAN
  // JOIN.
  static SqlSource FromSpanJoin(std::string sql,
                                const std::string& span_join_table);

  // Creates a SqlSource instance with the SQL taken as a substring starting at
  // |offset| with |len| characters.
  SqlSource Substr(uint32_t offset, uint32_t len) const;

  // Returns the this SqlSource instance as a string which can be appended as a
  // "traceback" frame to an error message. Callers can pass an optional
  // |offset| parameter which indicates the exact location of the error in the
  // SQL string.
  //
  // Specifically, this string will include:
  //  a) context about the source of the SQL
  //  b) line and column number of the error
  //  c) a snippet of the SQL and a caret (^) character pointing to the location
  //     of the error.
  std::string AsTracebackFrame(std::optional<uint32_t> offset) const;

  // Returns the SQL backing this SqlSource instance;
  const std::string& sql() const { return sql_; }

  bool operator==(const SqlSource& other) const {
    return std::tie(sql_, line_, col_) ==
           std::tie(other.sql_, other.line_, other.col_);
  }

 private:
  SqlSource(std::string sql,
            std::string name,
            bool include_traceback_header,
            uint32_t line,
            uint32_t col);

  std::string sql_;
  std::string name_;
  bool include_traceback_header_ = false;
  uint32_t line_ = 1;
  uint32_t col_ = 1;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQL_SOURCE_H_
