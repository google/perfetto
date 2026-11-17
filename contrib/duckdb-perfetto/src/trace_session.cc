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

#include "contrib/duckdb-perfetto/src/trace_session.h"

#include <cinttypes>
#include <cstdio>
#include <utility>

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto::duckdb_ext {
namespace {

using trace_processor::SqlValue;

std::optional<ColumnType> DeclTypeToColumnType(std::string decl) {
  decl = base::ToUpper(decl);
  if (decl.find("INT") != std::string::npos || decl == "BOOL")
    return ColumnType::kInt64;
  if (decl.find("REAL") != std::string::npos ||
      decl.find("DOUB") != std::string::npos ||
      decl.find("FLOA") != std::string::npos)
    return ColumnType::kDouble;
  if (decl.find("BLOB") != std::string::npos ||
      decl.find("BYTES") != std::string::npos)
    return ColumnType::kBytes;
  if (decl.find("TEXT") != std::string::npos ||
      decl.find("STRING") != std::string::npos ||
      decl.find("CHAR") != std::string::npos)
    return ColumnType::kString;
  return std::nullopt;
}

struct TypeVotes {
  bool l = false, d = false, s = false, b = false;
  void Add(SqlValue::Type t) {
    l |= t == SqlValue::kLong;
    d |= t == SqlValue::kDouble;
    s |= t == SqlValue::kString;
    b |= t == SqlValue::kBytes;
  }
  ColumnType Get() const {
    if (b && !l && !d && !s)
      return ColumnType::kBytes;
    if (s || b)
      return ColumnType::kString;
    if (d)
      return ColumnType::kDouble;
    if (l)
      return ColumnType::kInt64;
    // All NULL: text is the only lossless choice.
    return ColumnType::kString;
  }
};

bool IsIdentifier(const std::string& s) {
  if (s.empty())
    return false;
  for (char c : s) {
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_'))
      return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------

Trace::Trace() = default;
Trace::~Trace() = default;

base::StatusOr<std::shared_ptr<Trace>> Trace::Load(const std::string& path) {
  std::shared_ptr<Trace> t(new Trace());
  t->path_ = path;
  trace_processor::Config config;
  t->tp_ = trace_processor::TraceProcessor::CreateInstance(config);
  RETURN_IF_ERROR(trace_processor::ReadTrace(t->tp_.get(), path.c_str()));
  return t;
}

base::StatusOr<std::vector<std::string>> Trace::ListTables() {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = tp_->ExecuteQuery(
      "SELECT name FROM sqlite_master WHERE type IN ('table', 'view') "
      "AND name NOT LIKE '\\_%' ESCAPE '\\' ORDER BY name");
  std::vector<std::string> names;
  while (it.Next())
    names.push_back(it.Get(0).AsString());
  RETURN_IF_ERROR(it.Status());
  return names;
}

base::StatusOr<std::vector<ColumnInfo>> Trace::DescribeTable(
    const std::string& table) {
  std::vector<ColumnInfo> cols;
  if (!IsIdentifier(table))
    return cols;
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::optional<ColumnType>> declared;
  auto it = tp_->ExecuteQuery("SELECT name, type FROM pragma_table_info('" +
                              table + "')");
  while (it.Next()) {
    cols.push_back({it.Get(0).AsString(), ColumnType::kString});
    SqlValue decl = it.Get(1);
    declared.push_back(decl.is_null() ? std::nullopt
                                      : DeclTypeToColumnType(decl.AsString()));
  }
  RETURN_IF_ERROR(it.Status());

  for (size_t i = 0; i < cols.size(); ++i) {
    if (declared[i]) {
      cols[i].type = *declared[i];
      continue;
    }
    // Views (e.g. stdlib) often lack declared types: peek at a value.
    const std::string& c = cols[i].name;
    auto peek = tp_->ExecuteQuery("SELECT \"" + c + "\" FROM " + table +
                                  " WHERE \"" + c + "\" IS NOT NULL LIMIT 1");
    TypeVotes votes;
    if (peek.Next())
      votes.Add(peek.Get(0).type);
    if (!peek.Status().ok()) {
      // Table-valued functions (e.g. ancestor_slice) cannot be scanned
      // without arguments: report them as non-existent.
      return std::vector<ColumnInfo>();
    }
    cols[i].type = votes.Get();
  }
  return cols;
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

struct Cursor::OwnedValue {
  SqlValue::Type type = SqlValue::kNull;
  int64_t l = 0;
  double d = 0;
  std::string s;

  explicit OwnedValue(const SqlValue& v) : type(v.type) {
    switch (v.type) {
      case SqlValue::kLong:
        l = v.long_value;
        break;
      case SqlValue::kDouble:
        d = v.double_value;
        break;
      case SqlValue::kString:
        s = v.string_value;
        break;
      case SqlValue::kBytes:
        s.assign(static_cast<const char*>(v.bytes_value), v.bytes_count);
        break;
      case SqlValue::kNull:
        break;
    }
  }

  SqlValue View() const {
    switch (type) {
      case SqlValue::kLong:
        return SqlValue::Long(l);
      case SqlValue::kDouble:
        return SqlValue::Double(d);
      case SqlValue::kString:
        return SqlValue::String(s.c_str());
      case SqlValue::kBytes:
        return SqlValue::Bytes(s.data(), s.size());
      case SqlValue::kNull:
        break;
    }
    return SqlValue();
  }
};

Cursor::Cursor() = default;
Cursor::~Cursor() {
  // The iterator must be destroyed under the trace lock.
  std::lock_guard<std::mutex> lock(trace_->mu());
  it_.reset();
}

base::StatusOr<std::unique_ptr<Cursor>> Cursor::Open(
    std::shared_ptr<Trace> trace,
    const std::string& sql) {
  std::unique_ptr<Cursor> c(new Cursor());
  c->trace_ = std::move(trace);
  std::lock_guard<std::mutex> lock(c->trace_->mu());
  c->it_.emplace(c->trace_->tp()->ExecuteQuery(sql));
  RETURN_IF_ERROR(c->it_->Status());
  for (uint32_t i = 0; i < c->it_->ColumnCount(); ++i)
    c->names_.push_back(c->it_->GetColumnName(i));
  return std::move(c);
}

base::StatusOr<std::vector<ColumnType>> Cursor::InferTypes(size_t n) {
  {
    std::lock_guard<std::mutex> lock(trace_->mu());
    while (!exhausted_ && buffered_.size() < n) {
      if (!it_->Next()) {
        exhausted_ = true;
        RETURN_IF_ERROR(it_->Status());
        break;
      }
      std::vector<OwnedValue> row;
      row.reserve(names_.size());
      for (uint32_t c = 0; c < names_.size(); ++c)
        row.emplace_back(it_->Get(c));
      buffered_.push_back(std::move(row));
    }
  }
  std::vector<ColumnType> types;
  for (size_t c = 0; c < names_.size(); ++c) {
    TypeVotes votes;
    for (const auto& row : buffered_)
      votes.Add(row[c].type);
    types.push_back(votes.Get());
  }
  return types;
}

base::StatusOr<size_t> Cursor::Read(size_t max_rows, RowSink* sink) {
  size_t n = 0;
  uint32_t ncols = static_cast<uint32_t>(names_.size());
  for (; n < max_rows && buffered_pos_ < buffered_.size(); ++n) {
    const auto& row = buffered_[buffered_pos_++];
    for (uint32_t c = 0; c < ncols; ++c)
      RETURN_IF_ERROR(sink->Write(n, c, row[c].View()));
  }
  if (buffered_pos_ == buffered_.size() && !buffered_.empty()) {
    buffered_.clear();
    buffered_pos_ = 0;
  }
  if (n == max_rows || exhausted_)
    return n;
  std::lock_guard<std::mutex> lock(trace_->mu());
  for (; n < max_rows; ++n) {
    if (!it_->Next()) {
      exhausted_ = true;
      RETURN_IF_ERROR(it_->Status());
      break;
    }
    for (uint32_t c = 0; c < ncols; ++c)
      RETURN_IF_ERROR(sink->Write(n, c, it_->Get(c)));
  }
  return n;
}

std::string ToText(const SqlValue& v) {
  char buf[64];
  switch (v.type) {
    case SqlValue::kString:
      return v.string_value;
    case SqlValue::kBytes:
      return std::string(static_cast<const char*>(v.bytes_value),
                         v.bytes_count);
    case SqlValue::kLong:
      snprintf(buf, sizeof(buf), "%" PRId64, v.long_value);
      return buf;
    case SqlValue::kDouble:
      snprintf(buf, sizeof(buf), "%.17g", v.double_value);
      return buf;
    case SqlValue::kNull:
      break;
  }
  return "";
}

}  // namespace perfetto::duckdb_ext
