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
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

struct sqlite3_stmt;

namespace perfetto::trace_processor::exec {

// A pipeline's rows, read from a SQL query.
//
// Where a pipeline meets everything which is already SQL. The query's rows
// arrive a batch at a time, so nothing downstream can tell that SQLite
// produced them, and nothing downstream has to be taught to.
//
// It promises nothing about the order rows arrive in, because SQLite promises
// nothing either. Putting rows in an order is the business of whichever stage
// needs one.
class SqlScan : public core::exec::Source {
 public:
  static base::StatusOr<std::unique_ptr<SqlScan>> Create(SqliteConnection*,
                                                         SqlSource,
                                                         StringPool*);
  ~SqlScan() override;

  // The query's columns, in the order a batch carries them. Known before the
  // first row, because a stage has to resolve the names it was given against
  // something before it can start.
  const std::vector<std::string>& column_names() const { return names_; }

  void Reset() override;
  core::exec::RowBatch* Next() override;
  base::Status status() const override { return status_; }

 private:
  // One column's values for the batch being filled.
  //
  // A column's type is settled by the first non-null value seen in it and
  // cannot change afterwards: by the time a second batch is read the first
  // has gone downstream, and what has already left cannot be retyped. A
  // column which is null the whole way through settles as an integer holding
  // nothing.
  //
  // This is a stopgap. Inference exists because a bare query is all this is
  // given; once a FROM stage is built from a logical plan it will be handed
  // the column types and this will do as it is told.
  struct Column {
    std::optional<core::StorageType> type;
    // Only the buffer matching `type` is ever allocated.
    std::vector<int64_t> ints;
    std::vector<double> doubles;
    std::vector<StringPool::Id> strings;
    core::BitVector validity;
  };

  SqlScan(SqliteConnection::PreparedStatement,
          std::vector<std::string> names,
          StringPool*);

  // Reads one value into row `row` of column `index`. False on failure, with
  // `status_` saying why.
  bool ReadValue(sqlite3_stmt*, uint32_t index, uint32_t row);

  // Settles column `index` on `type`, allocating its buffer, or fails if the
  // column already settled on something else.
  bool Settle(uint32_t index, core::StorageType type);

  // The view of `column` a batch reads through.
  core::exec::ColumnView MakeView(const Column& column) const;

  SqliteConnection::PreparedStatement statement_;
  std::vector<std::string> names_;
  StringPool* pool_;

  // Fixed at construction: a batch's columns point into these, so they must
  // not move.
  std::vector<Column> columns_;
  // The columns as first built, each holding the row view a batch starts from.
  std::vector<core::exec::ColumnView> pristine_;
  core::exec::RowBatch batch_;

  bool done_ = false;
  base::Status status_ = base::OkStatus();
};

}  // namespace perfetto::trace_processor::exec

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_SQL_SCAN_H_
