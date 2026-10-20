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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PLAN_CURSOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PLAN_CURSOR_H_

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"

namespace perfetto::trace_processor {

// Runs a pipeline's plan for a SQLite virtual table's cursor: casts the values
// SQLite passes for the plan's parameters into a copy of its filter values,
// then hands SQLite the rows a cell at a time.
class PlanCursor {
 public:
  // Gives SQLite the value of `column` of the batch at `row`.
  using ResultFn = void (*)(sqlite3_context*,
                            StringPool*,
                            const core::exec::RowBatch& batch,
                            uint32_t column,
                            uint32_t row);
  // How a column SQLite reads is read: picked once, as every batch of a plan
  // lays its columns out the same way.
  struct Reader {
    // Where the column is in the plan's batches.
    uint32_t column = 0;
    ResultFn result = nullptr;
  };

  // `pool` holds the plan's strings.
  PlanCursor(std::unique_ptr<pipeline::PhysicalPlan> plan, StringPool* pool);
  ~PlanCursor();
  PlanCursor(const PlanCursor&) = delete;
  PlanCursor& operator=(const PlanCursor&) = delete;

  const pipeline::PhysicalPlan& plan() const { return *plan_; }

  // How the plan's `output`th column is read.
  Reader OutputReader(uint32_t output) const;

  // Casts the values of the plan's parameters, the `n`th at `params[n]`, for
  // the columns they are compared with, then moves to the first row. False
  // when there is none.
  bool Run(sqlite3_value** params);

  // Moves to the next row. False when there is none.
  bool Next() { return rows_.Next(); }

  // Why the rows stopped, once they have.
  base::Status status() const { return rows_.status(); }

  // Gives SQLite the value `reader` reads at the current row.
  void Result(sqlite3_context* ctx, const Reader& reader) const {
    reader.result(ctx, pool_, rows_.batch(), reader.column, rows_.row());
  }

  // The value of an Id column at the current row.
  uint32_t Id(uint32_t column) const { return rows_.Value<uint32_t>(column); }

 private:
  // How a parameter is cast before each run, picked when the cursor is made:
  // by its column's type and comparison, so the values most often passed need
  // no dispatch on either.
  struct Binder;
  using BindFn = void (*)(const Binder&, sqlite3_value** params, StringPool*);
  struct Binder {
    BindFn bind = nullptr;
    const pipeline::PhysicalPlan::Param* param = nullptr;
    core::exec::FilterValue* value = nullptr;
  };
  static BindFn BinderFor(const pipeline::PhysicalPlan::Param&);
  static void BindAny(const Binder&, sqlite3_value** params, StringPool*);
  template <typename T>
  static void BindIntegerEq(const Binder&, sqlite3_value** params, StringPool*);

  std::unique_ptr<pipeline::PhysicalPlan> plan_;
  StringPool* pool_;
  // The plan's filter values, with its parameters cast into them each run.
  std::vector<core::exec::FilterValue> filter_values_;
  // By parameter.
  std::vector<Binder> binders_;
  core::exec::RowCursor rows_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PLAN_CURSOR_H_
