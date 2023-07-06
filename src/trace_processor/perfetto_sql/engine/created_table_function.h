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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_CREATED_TABLE_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_CREATED_TABLE_FUNCTION_H_

#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"

namespace perfetto {
namespace trace_processor {

struct CreatedTableFunctionContext {
  PerfettoSqlEngine* engine = nullptr;

  Prototype prototype;
  std::vector<sql_argument::ArgumentDefinition> return_values;

  std::string prototype_str;
  std::string sql_defn_str;
};

class CreatedTableFunction final
    : public TypedSqliteTable<CreatedTableFunction,
                              CreatedTableFunctionContext> {
 public:
  class Cursor final : public SqliteTable::BaseCursor {
   public:
    explicit Cursor(CreatedTableFunction* table);
    ~Cursor() final;

    base::Status Filter(const QueryConstraints& qc,
                        sqlite3_value**,
                        FilterHistory);
    base::Status Next();
    bool Eof();
    base::Status Column(sqlite3_context* context, int N);

   private:
    std::optional<SqliteEngine::PreparedStatement> stmt_;
    CreatedTableFunction* table_ = nullptr;
    bool is_eof_ = false;
    int next_call_count_ = 0;
  };

  CreatedTableFunction(sqlite3*, CreatedTableFunctionContext);
  ~CreatedTableFunction() final;

  base::Status Init(int argc, const char* const* argv, Schema*) final;
  std::unique_ptr<SqliteTable::BaseCursor> CreateCursor() final;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) final;

  base::StatusOr<SqliteEngine::PreparedStatement> GetOrCreateStatement();
  void ReturnStatementForReuse(SqliteEngine::PreparedStatement stmt);

 private:
  Schema CreateSchema();

  bool IsReturnValueColumn(size_t i) const {
    PERFETTO_DCHECK(i < schema().columns().size());
    return i < context_.return_values.size();
  }

  bool IsArgumentColumn(size_t i) const {
    PERFETTO_DCHECK(i < schema().columns().size());
    return i >= context_.return_values.size() &&
           (i - context_.return_values.size()) <
               context_.prototype.arguments.size();
  }

  bool IsPrimaryKeyColumn(size_t i) const {
    PERFETTO_DCHECK(i < schema().columns().size());
    return i == (context_.return_values.size() +
                 context_.prototype.arguments.size());
  }

  CreatedTableFunctionContext context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_CREATED_TABLE_FUNCTION_H_
