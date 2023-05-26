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

#include "src/trace_processor/sqlite/perfetto_sql_engine.h"

#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace {

void IncrementCountForStmt(sqlite3_stmt* stmt,
                           PerfettoSqlEngine::ExecutionResult* res) {
  res->statement_count++;

  // If the stmt is already done, it clearly didn't have any output.
  if (sqlite_utils::IsStmtDone(stmt))
    return;

  if (sqlite3_column_count(stmt) == 1) {
    sqlite3_value* value = sqlite3_column_value(stmt, 0);

    // If the "VOID" pointer associated to the return value is not null,
    // that means this is a function which is forced to return a value
    // (because all functions in SQLite have to) but doesn't actually
    // wait to (i.e. it wants to be treated like CREATE TABLE or similar).
    // Because of this, ignore the return value of this function.
    // See |WrapSqlFunction| for where this is set.
    if (sqlite3_value_pointer(value, "VOID") != nullptr) {
      return;
    }

    // If the statement only has a single column and that column is named
    // "suppress_query_output", treat it as a statement without output for
    // accounting purposes. This allows an escape hatch for cases where the
    // user explicitly wants to ignore functions as having output.
    if (strcmp(sqlite3_column_name(stmt, 0), "suppress_query_output") == 0) {
      return;
    }
  }

  // Otherwise, the statement has output and so increment the count.
  res->statement_count_with_output++;
}

}  // namespace

PerfettoSqlEngine::PerfettoSqlEngine() : query_cache_(new QueryCache()) {}

void PerfettoSqlEngine::RegisterTable(const Table& table,
                                      const std::string& table_name) {
  DbSqliteTable::Context context{query_cache_.get(),
                                 DbSqliteTable::TableComputation::kStatic,
                                 &table, nullptr};
  engine_.RegisterVirtualTableModule<DbSqliteTable>(
      table_name, std::move(context), SqliteTable::kEponymousOnly, false);

  // Register virtual tables into an internal 'perfetto_tables' table.
  // This is used for iterating through all the tables during a database
  // export.
  char* insert_sql = sqlite3_mprintf(
      "INSERT INTO perfetto_tables(name) VALUES('%q')", table_name.c_str());
  char* error = nullptr;
  sqlite3_exec(engine_.db(), insert_sql, nullptr, nullptr, &error);
  sqlite3_free(insert_sql);
  if (error) {
    PERFETTO_ELOG("Error adding table to perfetto_tables: %s", error);
    sqlite3_free(error);
  }
}

void PerfettoSqlEngine::RegisterTableFunction(
    std::unique_ptr<TableFunction> fn) {
  std::string table_name = fn->TableName();
  DbSqliteTable::Context context{query_cache_.get(),
                                 DbSqliteTable::TableComputation::kDynamic,
                                 nullptr, std::move(fn)};
  engine_.RegisterVirtualTableModule<DbSqliteTable>(
      table_name, std::move(context), SqliteTable::kEponymousOnly, false);
}

base::StatusOr<PerfettoSqlEngine::ExecutionResult>
PerfettoSqlEngine::ExecuteUntilLastStatement(const std::string& sql) {
  ExecutionResult res;

  // A sql string can contain several statements. Some of them might be comment
  // only, e.g. "SELECT 1; /* comment */; SELECT 2;". Here we process one
  // statement on each iteration. SQLite's sqlite_prepare_v2 (wrapped by
  // PrepareStmt) returns on each iteration a pointer to the unprocessed string.
  //
  // Unfortunately we cannot call PrepareStmt and tokenize all statements
  // upfront because sqlite_prepare_v2 also semantically checks the statement
  // against the schema. In some cases statements might depend on the execution
  // of previous ones (e.e. CREATE VIEW x; SELECT FROM x; DELETE VIEW x;).
  //
  // Also, unfortunately, we need to PrepareStmt to find out if a statement is a
  // comment or a real statement.
  //
  // The logic here is the following:
  //  - We invoke PrepareStmt on each statement.
  //  - If the statement is a comment we simply skip it.
  //  - If the statement is valid, we step once to make sure side effects take
  //    effect.
  //  - If we encounter a valid statement afterwards, we step internally through
  //    all rows of the previous one. This ensures that any further side effects
  //    take hold *before* we step into the next statement.
  //  - Once no further non-comment statements are encountered, we return an
  //    iterator to the last valid statement.
  for (const char* rem_sql = sql.c_str(); rem_sql && rem_sql[0];) {
    ScopedStmt cur_stmt;
    {
      PERFETTO_TP_TRACE(metatrace::Category::QUERY, "QUERY_PREPARE");
      const char* tail = nullptr;
      RETURN_IF_ERROR(
          sqlite_utils::PrepareStmt(engine_.db(), rem_sql, &cur_stmt, &tail));
      rem_sql = tail;
    }

    // The only situation where we'd have an ok status but also no prepared
    // statement is if the statement of SQL we parsed was a pure comment. In
    // this case, just continue to the next statement.
    if (!cur_stmt)
      continue;

    // Before stepping into |cur_stmt|, we need to finish iterating through
    // the previous statement so we don't have two clashing statements (e.g.
    // SELECT * FROM v and DROP VIEW v) partially stepped into.
    if (res.stmt) {
      PERFETTO_TP_TRACE(metatrace::Category::QUERY, "STMT_STEP_UNTIL_DONE",
                        [&res](metatrace::Record* record) {
                          auto expanded_sql =
                              sqlite_utils::ExpandedSqlForStmt(res.stmt.get());
                          record->AddArg("SQL", expanded_sql.get());
                        });
      RETURN_IF_ERROR(sqlite_utils::StepStmtUntilDone(res.stmt.get()));
      res.stmt.reset();
    }

    PERFETTO_DLOG("Executing statement: %s", sqlite3_sql(*cur_stmt));

    {
      PERFETTO_TP_TRACE(metatrace::Category::TOPLEVEL, "STMT_FIRST_STEP",
                        [&cur_stmt](metatrace::Record* record) {
                          auto expanded_sql =
                              sqlite_utils::ExpandedSqlForStmt(*cur_stmt);
                          record->AddArg("SQL", expanded_sql.get());
                        });

      // Now step once into |cur_stmt| so that when we prepare the next statment
      // we will have executed any dependent bytecode in this one.
      int err = sqlite3_step(*cur_stmt);
      if (err != SQLITE_ROW && err != SQLITE_DONE) {
        return base::ErrStatus(
            "%s", sqlite_utils::FormatErrorMessage(
                      cur_stmt.get(), base::StringView(sql), engine_.db(), err)
                      .c_message());
      }
    }

    // Increment the neecessary counts for the statement.
    IncrementCountForStmt(cur_stmt.get(), &res);

    // Propogate the current statement to the next iteration.
    res.stmt = std::move(cur_stmt);
  }

  // If we didn't manage to prepare a single statment, that means everything
  // in the SQL was treated as a comment.
  if (!res.stmt)
    return base::ErrStatus("No valid SQL to run");

  // Update the output statment and column count.
  res.column_count =
      static_cast<uint32_t>(sqlite3_column_count(res.stmt.get()));
  return std::move(res);
}

}  // namespace trace_processor
}  // namespace perfetto
