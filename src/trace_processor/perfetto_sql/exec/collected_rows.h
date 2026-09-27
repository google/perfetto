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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_COLLECTED_ROWS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_COLLECTED_ROWS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

struct sqlite3_value;

namespace perfetto::trace_processor::exec {

// The pointer type SQLite passes collected rows under: a
// std::shared_ptr<const CollectedRows>.
inline constexpr char kCollectedRowsPointerType[] = "perfetto_collected_rows";

// A relation SQLite hands to a pipeline: rows collected into batches.
//
// A column whose type is known holds values of that type or nulls; any other
// column carries its type per row. SQLite's declared types establish nothing,
// so a type is only known when the column traces back to a dataframe column.
class CollectedRows {
 public:
  CollectedRows(core::Schema columns, StringPool*);
  ~CollectedRows();
  CollectedRows(const CollectedRows&) = delete;
  CollectedRows& operator=(const CollectedRows&) = delete;

  // Appends a row: one value for each column. Fails on a value a pipeline
  // cannot carry, such as a blob, which leaves the rows unusable.
  base::Status Append(sqlite3_value** values);

  const core::Schema& columns() const { return columns_; }
  uint32_t batch_count() const {
    return static_cast<uint32_t>(batches_.size());
  }

  // Views batch `i`. The rows must outlive the view.
  void View(uint32_t i, core::exec::RowBatch& out) const;

 private:
  struct Batch {
    std::vector<std::shared_ptr<core::exec::ColumnChunk>> columns;
    // Each column's value buffer, resolved out of its chunk once.
    std::vector<void*> data;
    uint32_t count = 0;
  };

  void AddBatch();
  base::Status Read(Batch&, uint32_t column, sqlite3_value*);
  template <typename T, int kSqliteType>
  base::Status ReadTyped(Batch&, uint32_t column, sqlite3_value*);

  core::Schema columns_;
  StringPool* pool_;
  std::vector<Batch> batches_;
};

// Reads the rows of the input a run is given at `index`.
class CollectedRowsScan : public core::exec::Source {
 public:
  // The rows each run reads, one entry per input. Whoever runs the plan sets
  // them before each run.
  using Inputs = std::vector<std::shared_ptr<const CollectedRows>>;

  // `inputs` must outlive this.
  CollectedRowsScan(const Inputs& inputs, uint32_t index);
  ~CollectedRowsScan() override;

  std::unique_ptr<core::exec::OperatorState> MakeState() const override;
  bool GetData(core::exec::RowBatch& out,
               core::exec::OperatorState&) const override;
  void Rewind(core::exec::OperatorState&) const override;
  base::Status status(const core::exec::OperatorState&) const override;

 private:
  struct State : core::exec::OperatorState {
    ~State() override;
    uint32_t next_batch = 0;
  };

  const Inputs& inputs_;
  uint32_t index_;
};

}  // namespace perfetto::trace_processor::exec

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_EXEC_COLLECTED_ROWS_H_
