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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_SQL_SCAN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_SQL_SCAN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

struct sqlite3_stmt;

namespace perfetto::trace_processor::exec {

// Reads a pipeline's rows from a SQL query.
//
// Promises nothing about the order the rows arrive in, because SQLite does
// not.
//
// Each column carries its type per row unless the query can be traced back to
// a dataframe column, which is the only way to establish a type. SQLite's
// declared types establish nothing: an INTEGER column holds text if something
// puts text in it.
class SqlScan : public core::exec::Source {
 public:
  // The catalog is where column types come from: any column it cannot trace
  // back is a variant.
  static base::StatusOr<std::unique_ptr<SqlScan>> Create(
      SqliteConnection*,
      SqlSource,
      StringPool*,
      const perfetto_sql::analysis::Catalog&);
  ~SqlScan() override;

  // The query's columns, in the order a batch carries them.
  const std::vector<std::string>& column_names() const { return names_; }

  // The type of column `i`, or nothing when the column carries a type per
  // row.
  std::optional<core::StorageType> column_type(uint32_t i) const {
    return types_[i];
  }

  std::unique_ptr<core::exec::OperatorState> MakeState() const override;
  bool GetData(core::exec::RowBatch& out,
               core::exec::OperatorState&) const override;
  void Rewind(core::exec::OperatorState&) const override;
  base::Status status(const core::exec::OperatorState&) const override;

 private:
  struct State : core::exec::OperatorState {
    ~State() override;
    std::optional<SqliteConnection::PreparedStatement> statement;
    // Shared so a batch can keep the values alive.
    std::vector<std::shared_ptr<core::exec::ColumnChunk>> columns;
    // Each column's value buffer, resolved out of its chunk once.
    std::vector<void*> data;
    bool done = false;
    base::Status status = base::OkStatus();
  };

  SqlScan(SqliteConnection*,
          SqlSource,
          std::vector<std::string>,
          std::vector<std::optional<core::StorageType>>,
          StringPool*);
  void Prepare(State&) const;

  bool ReadValue(State&, sqlite3_stmt*, uint32_t index, uint32_t row) const;
  template <typename T, int SqliteType>
  bool ReadTypedValue(State&,
                      sqlite3_stmt*,
                      uint32_t index,
                      uint32_t row) const;

  SqliteConnection* connection_;
  SqlSource sql_;
  std::vector<std::string> names_;
  std::vector<std::optional<core::StorageType>> types_;
  StringPool* pool_;
};

}  // namespace perfetto::trace_processor::exec

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_SQL_SCAN_H_
