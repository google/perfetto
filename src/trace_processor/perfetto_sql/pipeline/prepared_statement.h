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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PREPARED_STATEMENT_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PREPARED_STATEMENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/perfetto_sql/pipeline/iterator.h"

namespace perfetto::trace_processor {
class StringPool;
namespace core::dataframe {
class Dataframe;
}  // namespace core::dataframe
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

class Builder;
class Catalog;
class LogicalPlan;
class PlanKey;

// Runtime inputs required to execute a PreparedStatement.
struct ExecutionContext {
  // Backing pool for string columns and literals (required).
  StringPool* pool = nullptr;
  // Resolves named tables from Builder::ScanTable; may be null if unused.
  const Catalog* catalog = nullptr;
  // Required when executing a statement loaded via Deserialize(bytes, key).
  const PlanKey* plan_key = nullptr;
  // Positional dataframe inputs for Builder::Scan and hoisted SQL sources.
  base::Span<const core::dataframe::Dataframe* const> args;
};

// An immutable pipeline statement ready to serialize or execute.
//
// Created via `pipeline::Builder::Finish()` or `Deserialize()`. Move-only;
// may be serialized and executed any number of times, or consumed via rvalue
// overloads (`std::move(stmt).ExecuteRows(...)` / `ToSqliteSql(...)`) to avoid
// cloning the underlying plan.
class PreparedStatement {
 public:
  PreparedStatement(const PreparedStatement&) = delete;
  PreparedStatement& operator=(const PreparedStatement&) = delete;
  PreparedStatement(PreparedStatement&&) noexcept;
  PreparedStatement& operator=(PreparedStatement&&) noexcept;
  ~PreparedStatement();

  // Unstamped host-endian serialization for trusted, same-process transport.
  std::string Serialize() const;
  static PreparedStatement Deserialize(std::string_view bytes);

  // Authenticated serialization using `key` for untrusted carriers (e.g. SQL).
  // Statements loaded this way require `key` in ExecutionContext::plan_key.
  std::string Serialize(const PlanKey& key) const;
  static base::StatusOr<PreparedStatement> Deserialize(std::string_view bytes,
                                                       const PlanKey& key);

  // Wraps this statement as SQLite SQL via `__intrinsic_pipeline(...)`,
  // authenticating with `key` and hoisting SQL sources into subquery args.
  base::StatusOr<std::string> ToSqliteSql(const PlanKey& key) &&;

  // Executes this statement, yielding zero-copy batches (default).
  base::StatusOr<BatchIterator> Execute(const ExecutionContext& ctx) const&;
  base::StatusOr<BatchIterator> Execute(const ExecutionContext& ctx) &&;

  // Executes this statement one row at a time (e.g. for SQLite virtual tables).
  base::StatusOr<RowIterator> ExecuteRows(const ExecutionContext& ctx) const&;
  base::StatusOr<RowIterator> ExecuteRows(const ExecutionContext& ctx) &&;

  uint32_t column_count() const;
  const std::string& column_name(uint32_t index) const;

 private:
  friend class Builder;
  // Test-only helpers (not linked into production binaries).
  friend class TestCatalog;
  friend std::string PreparedStatementToString(const PreparedStatement&);

  explicit PreparedStatement(std::unique_ptr<LogicalPlan> plan,
                             bool require_table_stamp = false);

  std::unique_ptr<LogicalPlan> plan_;
  // Enforces passing ExecutionContext::plan_key so table stamps are verified.
  bool require_table_stamp_ = false;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PREPARED_STATEMENT_H_
