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

// SPIKE v2: the dataframe module runs the dataframe planner's plans in the
// executor, when every operation in them can be. Not meant to land.

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_PIPELINE_SPIKE2_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_PIPELINE_SPIKE2_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/spike_rows.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor::pipeline_spike2 {

bool Enabled();
void CountFilter(bool pipeline);

// SPIKE: time spent per phase, when DF_TIMING is set.
enum Bucket {
  kPrepare,  // xFilter with a new plan: decoding it, building the cursor.
  kExecute,  // xFilter running a query.
  kNext,
  kEof,
  kColumn,
  kRowid,
  kCastNarrow,  // Inside kExecute: casting values and narrowing.
  kFirstBatch,  // Inside kExecute: rewinding and producing the first batch.
  kRefill,      // Inside kNext: producing a later batch.
  kBucketCount,
};
extern const bool g_timing;
inline bool TimingEnabled() {
  return g_timing;
}
uint64_t Now();
void AddTime(Bucket, uint64_t start);
// Compiled out unless SPIKE_TIMING is defined, so it costs nothing when off.
#if defined(SPIKE_TIMING)
struct ScopedTime {
  explicit ScopedTime(Bucket b)
      : bucket(b), start(TimingEnabled() ? Now() : 0) {}
  ~ScopedTime() {
    if (start) {
      AddTime(bucket, start);
    }
  }
  Bucket bucket;
  uint64_t start;
};
#else
struct ScopedTime {
  explicit ScopedTime(Bucket) {}
};
#endif

// Keeps `plan` if the executor can run all of it, returning its id, or -1.
int Register(const core::dataframe::Dataframe&, core::dataframe::LogicalPlan);
std::string Encode(int id, const std::string& plan);
// Returns where the interpreter's plan starts; `*id` is -1 without a plan.
const char* Decode(const char* idx_str, int* id);
#if defined(SPIKE_AUDIT)
// Counts a run of a plan the executor was not given.
void AuditInterpreterRun(const char* idx_str);
#endif

// Reads the values a query is run with from SQLite.
struct Fetcher;
// Casts a predicate's value as the column's type and the comparison need,
// both fixed when the plan is built.
using Caster = void (*)(uint32_t index,
                        sqlite3_value** argv,
                        core::filter::CastFilterValueResult* out);

// What an integer argument in range of the column's type is cast as, with no
// call: the case of every integer column, whatever the comparison.
enum class IntegerCast : uint8_t { kNone, kId, kUint32, kInt32, kInt64 };

// A dataframe column, flat: everything reading a cell of it needs, fixed when
// the plan is built, so a read goes from the cell to the value.
struct Cell;
using Reader = void (*)(sqlite3_context*,
                        const Cell&,
                        uint32_t index,
                        const StringPool*);
struct Cell {
  Reader read = nullptr;
  const void* data = nullptr;
  // The null bitvector's words, when the column has one.
  const uint64_t* validity = nullptr;
  // For a sparse column, the set bits before each word.
  const uint32_t* prefix = nullptr;
  // Where the column is in a batch.
  uint32_t position = 0;
};

class Query {
 public:
  Query(const core::dataframe::Dataframe&, int id);
  ~Query();

  void Execute(sqlite3_value** argv);
  // Inline, as SQLite calls them per row.
  void Next() {
    if (PERFETTO_LIKELY(++row_ < count_)) {
      index_ = rows_ ? rows_[row_] : offset_ + row_;
      return;
    }
    // A batch known to be the last needs no call to find there is no other.
    if (last_) {
      batch_ = nullptr;
      eof_ = true;
      return;
    }
    Advance();
  }
  bool Eof() const { return eof_; }
  void Column(sqlite3_context* ctx, uint32_t column) const {
    const Cell& cell = cells_data_[column];
    cell.read(ctx, cell, index_, pool_);
  }
  // The row's id is where it is in the table.
  int64_t Rowid(uint32_t) const { return index_; }

 private:
  // A predicate and where its value comes from.
  struct Predicate {
    uint32_t column;
    core::Op op;
    core::StorageType storage;
    int value_index;  // -1 for none.
  };
  struct Narrowing {
    Predicate predicate;
    core::exec::ColumnView view;
    core::exec::Narrower narrow = nullptr;
  };

  // A value a run casts, and what it casts it into.
  struct Binding {
    // Anything the inline cast does not cover.
    Caster cast;
    uint32_t index;
    IntegerCast integer;
    core::filter::CastFilterValueResult value;
  };

  void Advance();
  // Takes in batch_, just pulled: every column of it shares one selection,
  // resolved here once rather than per cell.
  void Arrived() {
    count_ = batch_ ? batch_->size() : 0;
    last_ = !batch_ || batch_->last();
    eof_ = batch_ == nullptr;
    if (batch_ && batch_->column_count()) {
      core::exec::RowSelection s = batch_->column(0).selection();
      rows_ = s.data();
      offset_ = s.offset();
      index_ = rows_ ? rows_[0] : offset_;
    }
  }

  // What every call reads, together at the front.
  // The pipeline's current batch, valid until the next pull.
  core::exec::RowBatch* batch_ = nullptr;
  uint32_t row_ = 0;
  uint32_t count_ = 0;
  // The current row's index in the table, and the selection it comes from.
  uint32_t index_ = 0;
  uint32_t offset_ = 0;
  const uint32_t* rows_ = nullptr;
  bool eof_ = true;
  // Whether batch_ is known to be the last.
  bool last_ = false;
  const Cell* cells_data_ = nullptr;  // cells_, by dataframe column.
  const StringPool* pool_ = nullptr;
  // What runs: the scan itself when there are no operators, else pipeline_.
  const core::exec::Source* runner_ = nullptr;
  core::exec::OperatorState* run_state_ = nullptr;  // state_.
  // Inline, so the values a run casts share the lines above.
  base::SmallVector<Binding, 2> bindings_;

  const core::dataframe::Dataframe& df_;
  bool empty_ = false;
  const uint32_t* permutation_ = nullptr;
  std::vector<core::Slab<uint32_t>> prefixes_;
  std::vector<Narrowing> narrowings_;
  std::vector<Predicate> filters_;
  std::vector<Cell> cells_;  // By dataframe column.
  std::unique_ptr<core::exec::RowsScan> scan_;
  std::unique_ptr<core::exec::Pipeline> pipeline_;
  std::unique_ptr<core::exec::OperatorState> state_;
  core::exec::RowBatch scratch_;
#if defined(SPIKE_AUDIT)
 public:
  // What the plan is, and what running it did, summed over its runs.
  struct Audit {
    uint64_t executes = 0;
    uint64_t batches = 0;
    uint64_t rows = 0;
  };
  std::string audit_plan_;
  Audit audit_;
#endif
};

}  // namespace perfetto::trace_processor::pipeline_spike2

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_PIPELINE_SPIKE2_H_
