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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_RUNTIME_TABLE_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_RUNTIME_TABLE_FUNCTION_H_

#include <optional>

#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"

namespace perfetto {
namespace trace_processor {

class PerfettoSqlEngine;

// The implementation of the SqliteTable interface for table functions defined
// at runtime using SQL.
class RuntimeTableFunction final
    : public TypedSqliteTable<RuntimeTableFunction, PerfettoSqlEngine*> {
 public:
  // The state of this function. This is separated from |RuntimeTableFunction|
  // because |RuntimeTableFunction| is owned by Sqlite while |State| is owned by
  // PerfettoSqlEngine.
  struct State {
    SqlSource sql_defn_str;

    FunctionPrototype prototype;
    std::vector<sql_argument::ArgumentDefinition> return_values;

    std::optional<SqliteEngine::PreparedStatement> reusable_stmt;

    bool IsReturnValueColumn(size_t i) const {
      PERFETTO_DCHECK(i < TotalColumnCount());
      return i < return_values.size();
    }

    bool IsArgumentColumn(size_t i) const {
      PERFETTO_DCHECK(i < TotalColumnCount());
      return i >= return_values.size() &&
             (i - return_values.size()) < prototype.arguments.size();
    }

    bool IsPrimaryKeyColumn(size_t i) const {
      PERFETTO_DCHECK(i < TotalColumnCount());
      return i == (return_values.size() + prototype.arguments.size());
    }

    size_t TotalColumnCount() const {
      static constexpr uint32_t kPrimaryKeyColumns = 1;
      return prototype.arguments.size() + return_values.size() +
             kPrimaryKeyColumns;
    }
  };
  class Cursor final : public SqliteTable::BaseCursor {
   public:
    explicit Cursor(RuntimeTableFunction* table, State* state);
    ~Cursor() final;

    base::Status Filter(const QueryConstraints& qc,
                        sqlite3_value**,
                        FilterHistory);
    base::Status Next();
    bool Eof();
    base::Status Column(sqlite3_context* context, int N);

   private:
    RuntimeTableFunction* table_ = nullptr;
    State* state_ = nullptr;

    std::optional<SqliteEngine::PreparedStatement> stmt_;
    bool return_stmt_to_state_ = false;

    bool is_eof_ = false;
    int next_call_count_ = 0;
  };

  RuntimeTableFunction(sqlite3*, PerfettoSqlEngine*);
  ~RuntimeTableFunction() final;

  base::Status Init(int argc, const char* const* argv, Schema*) final;
  std::unique_ptr<SqliteTable::BaseCursor> CreateCursor() final;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) final;

 private:
  Schema CreateSchema();

  PerfettoSqlEngine* engine_ = nullptr;
  State* state_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_RUNTIME_TABLE_FUNCTION_H_
