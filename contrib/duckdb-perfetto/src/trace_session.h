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

#ifndef CONTRIB_DUCKDB_PERFETTO_SRC_TRACE_SESSION_H_
#define CONTRIB_DUCKDB_PERFETTO_SRC_TRACE_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"

namespace perfetto::trace_processor {
class TraceProcessor;
}

namespace perfetto::duckdb_ext {

// The DuckDB-agnostic half of the extension: everything which talks to
// TraceProcessor. Compiled with Perfetto's usual flags (no exceptions).

enum class ColumnType { kInt64, kDouble, kString, kBytes };

struct ColumnInfo {
  std::string name;
  ColumnType type;
};

// A loaded trace. TraceProcessor is not thread-safe: every interaction goes
// through |mu|. Different traces are independent and can be used from
// different threads concurrently (this requires SQLite to be built with
// SQLITE_THREADSAFE=2; see .gn).
class Trace {
 public:
  static base::StatusOr<std::shared_ptr<Trace>> Load(const std::string& path);
  ~Trace();

  const std::string& path() const { return path_; }
  trace_processor::TraceProcessor* tp() { return tp_.get(); }
  std::mutex& mu() { return mu_; }

  // Public tables and views (not starting with '_').
  base::StatusOr<std::vector<std::string>> ListTables();

  // Schema of |table|: declared types where available, otherwise the type of
  // the first non-NULL value. Returns an empty vector if there is no such
  // table.
  base::StatusOr<std::vector<ColumnInfo>> DescribeTable(
      const std::string& table);

 private:
  Trace();

  std::string path_;
  std::unique_ptr<trace_processor::TraceProcessor> tp_;
  std::mutex mu_;
};

// Receives cells produced by Cursor::Read.
//
// Implemented on the DuckDB side, which is built with RTTI while this file's
// .cc is not: keep this class free of out-of-line key functions so that its
// typeinfo is emitted by the subclass's translation unit.
class RowSink {
 public:
  virtual ~RowSink() = default;
  // |row| is relative to the current Read() call.
  virtual base::Status Write(size_t row,
                             uint32_t col,
                             const trace_processor::SqlValue& value) = 0;
};

// A streaming query result.
class Cursor {
 public:
  // Runs |sql| against |trace|.
  static base::StatusOr<std::unique_ptr<Cursor>> Open(
      std::shared_ptr<Trace> trace,
      const std::string& sql);
  ~Cursor();

  const std::string& label() const { return trace_->path(); }
  const std::vector<std::string>& column_names() const { return names_; }

  // Pulls up to |n| rows into an internal buffer and infers a column type for
  // each column from them (SQLite results are dynamically typed). Columns
  // which are all NULL in the sample are reported as kString.
  base::StatusOr<std::vector<ColumnType>> InferTypes(size_t n);

  // Writes up to |max_rows| rows (buffered rows first) to |sink|.
  base::StatusOr<size_t> Read(size_t max_rows, RowSink* sink);

  bool done() const { return exhausted_ && buffered_pos_ >= buffered_.size(); }

 private:
  struct OwnedValue;

  Cursor();

  std::shared_ptr<Trace> trace_;
  std::optional<trace_processor::Iterator> it_;
  std::vector<std::string> names_;
  std::vector<std::vector<OwnedValue>> buffered_;
  size_t buffered_pos_ = 0;
  bool exhausted_ = false;
};

// Formats |value| as text (for kString columns receiving numbers).
std::string ToText(const trace_processor::SqlValue& value);

}  // namespace perfetto::duckdb_ext

#endif  // CONTRIB_DUCKDB_PERFETTO_SRC_TRACE_SESSION_H_
