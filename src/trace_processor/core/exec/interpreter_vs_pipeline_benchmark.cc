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

// SPIKE: the dataframe interpreter against hand-built pipelines for the query
// shapes the dataframe module runs most. Not meant to land as is.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr uint32_t kRows = 1000000;
constexpr uint32_t kUtidRun = 500;
constexpr uint32_t kTracks = 2000;

enum Col : uint32_t { kUtid, kTrack, kDur, kId };

// The table, and the same values as plain arrays for the pipeline to read.
struct Table {
  StringPool pool;
  std::unique_ptr<dataframe::Dataframe> df;
  std::vector<uint32_t> utid;
  std::vector<uint32_t> track;
  std::vector<int64_t> dur;
};

Table& GetTable() {
  static Table* table = [] {
    auto* t = new Table();
    std::mt19937 rng(1);
    dataframe::AdhocDataframeBuilder builder({"utid", "track", "dur"},
                                             &t->pool);
    for (uint32_t i = 0; i < kRows; ++i) {
      t->utid.push_back(i / kUtidRun);
      t->track.push_back(static_cast<uint32_t>(rng() % kTracks));
      t->dur.push_back(static_cast<int64_t>(rng() % 1000000) + (1ll << 40));
      PERFETTO_CHECK(builder.PushNonNull(kUtid, int64_t{t->utid.back()}));
      PERFETTO_CHECK(builder.PushNonNull(kTrack, int64_t{t->track.back()}));
      PERFETTO_CHECK(builder.PushNonNull(kDur, t->dur.back()));
    }
    auto df = std::move(builder).Build();
    PERFETTO_CHECK(df.ok());
    t->df = std::make_unique<dataframe::Dataframe>(std::move(*df));
    t->df->Finalize();
    return t;
  }();
  return *table;
}

// The filter value the SQL module would pass from sqlite3_value.
struct Fetcher : ValueFetcher {
  static const Type kInt64 = 0;
  static const Type kDouble = 1;
  static const Type kString = 2;
  static const Type kNull = 3;
  int64_t value = 0;
  int64_t GetInt64Value(uint32_t) const { return value; }
  double GetDoubleValue(uint32_t) const { return 0; }
  const char* GetStringValue(uint32_t) const { return nullptr; }
  Type GetValueType(uint32_t) const { return kInt64; }
  bool IteratorInit(uint32_t) const { return false; }
  bool IteratorNext(uint32_t) const { return false; }
};

struct Sum : dataframe::CellCallback {
  int64_t total = 0;
  void OnCell(int64_t v) { total += v; }
  void OnCell(double) {}
  void OnCell(NullTermStringView) {}
  void OnCell(std::nullptr_t) {}
  void OnCell(int32_t v) { total += v; }
  void OnCell(uint32_t v) { total += v; }
};

std::vector<int64_t> Keys(uint32_t count, uint32_t bound) {
  std::mt19937 rng(2);
  std::vector<int64_t> keys(count);
  for (int64_t& key : keys) {
    key = static_cast<int64_t>(rng() % bound);
  }
  return keys;
}

// Runs `filter_col = ?` [ORDER BY dur] through the interpreter, planned once
// and executed per key through one cursor, as the SQL module does.
void RunInterpreter(benchmark::State& state,
                    uint32_t filter_col,
                    bool sort,
                    uint32_t key_bound) {
  Table& t = GetTable();
  std::vector<dataframe::FilterSpec> filters = {
      {filter_col, 0, Eq{}, std::nullopt}};
  std::vector<dataframe::SortSpec> sorts;
  if (sort) {
    sorts.push_back({kDur, SortDirection::kAscending});
  }
  auto plan = t.df->PlanQuery(filters, {}, sorts, {}, 1ull << kDur);
  PERFETTO_CHECK(plan.ok());
  auto cursor = std::make_unique<dataframe::Cursor<Fetcher>>();
  t.df->PrepareCursor(*plan, *cursor);
  std::vector<int64_t> keys = Keys(4096, key_bound);
  Fetcher fetcher;
  uint32_t i = 0;
  int64_t rows = 0;
  for (auto _ : state) {
    fetcher.value = keys[i++ % keys.size()];
    cursor->Execute(fetcher);
    Sum sum;
    for (; !cursor->Eof(); cursor->Next()) {
      cursor->Cell(0, sum);
      ++rows;
    }
    benchmark::DoNotOptimize(sum.total);
  }
  state.counters["rows/query"] =
      static_cast<double>(rows) / static_cast<double>(state.iterations());
}

// SPIKE source: the rows whose sorted `column` equals the key, found by
// binary search and emitted as a range, with the dur column.
class SortedEqSource final : public Source {
 public:
  SortedEqSource(const std::vector<uint32_t>& column,
                 const std::vector<int64_t>& dur,
                 const int64_t* key)
      : column_(column), dur_(dur), key_(key) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().done = false;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.done) {
      return false;
    }
    s.done = true;
    auto value = static_cast<uint32_t>(*key_);
    auto lo = static_cast<uint32_t>(
        std::lower_bound(column_.begin(), column_.end(), value) -
        column_.begin());
    auto hi = static_cast<uint32_t>(
        std::upper_bound(column_.begin() + lo, column_.end(), value) -
        column_.begin());
    out.Reset();
    ColumnView dur = ColumnView::Reference(StorageType{Int64{}}, dur_.data());
    dur.SetRange(lo);
    out.AddColumn(dur);
    out.SetCardinality(hi - lo);
    return hi > lo;
  }

 private:
  struct State : OperatorState {
    bool done = false;
  };
  const std::vector<uint32_t>& column_;
  const std::vector<int64_t>& dur_;
  const int64_t* key_;
};

// SPIKE source: an id lookup, the one row whose index is the key.
class IdSource final : public Source {
 public:
  IdSource(const std::vector<int64_t>& dur, const int64_t* key)
      : dur_(dur), key_(key) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().done = false;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.done || *key_ < 0 || *key_ >= static_cast<int64_t>(dur_.size())) {
      return false;
    }
    s.done = true;
    out.Reset();
    ColumnView dur = ColumnView::Reference(StorageType{Int64{}}, dur_.data());
    dur.SetRange(static_cast<uint32_t>(*key_));
    out.AddColumn(dur);
    out.SetCardinality(1);
    return true;
  }

 private:
  struct State : OperatorState {
    bool done = false;
  };
  const std::vector<int64_t>& dur_;
  const int64_t* key_;
};

// SPIKE source: every row, a batch at a time, with track and dur.
class ScanSource final : public Source {
 public:
  ScanSource(const std::vector<uint32_t>& track,
             const std::vector<int64_t>& dur)
      : track_(track), dur_(dur) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(dur_.size());
    if (s.emitted == rows) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, rows - s.emitted);
    out.Reset();
    ColumnView track =
        ColumnView::Reference(StorageType{Uint32{}}, track_.data());
    ColumnView dur = ColumnView::Reference(StorageType{Int64{}}, dur_.data());
    track.SetRange(s.emitted);
    dur.SetRange(s.emitted);
    out.AddColumn(track);
    out.AddColumn(dur);
    out.SetCardinality(count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };
  const std::vector<uint32_t>& track_;
  const std::vector<int64_t>& dur_;
};

// SPIKE operator: keeps the rows whose Uint32 `column` equals the key.
template <bool kRangeFastPath>
class EqFilter final : public Operator {
 public:
  EqFilter(uint32_t column, const int64_t* key) : column_(column), key_(key) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override {
    State& s = state.Cast<State>();
    const ColumnView& column = in.column(column_);
    const auto* data = static_cast<const uint32_t*>(column.data());
    RowSelection selection = column.selection();
    auto value = static_cast<uint32_t>(*key_);
    s.rows.resize(kMaxBatchRows);
    uint32_t* w = s.rows.data();
    if (kRangeFastPath && selection.is_range()) {
      // Branchy on purpose, as the interpreter's filters are: few rows match.
      const uint32_t* base = data + selection.offset();
      for (uint32_t row = 0; row < in.size(); ++row) {
        if (base[row] == value) {
          *w++ = row;
        }
      }
    } else {
      for (uint32_t row = 0; row < in.size(); ++row) {
        *w = row;
        w += data[selection.GetIndex(row)] == value;
      }
    }
    auto count = static_cast<uint32_t>(w - s.rows.data());
    out.CopyFrom(in);
    if (count == 0) {
      out.SetCardinality(0);
      return OpResult::kNeedMoreInput;
    }
    out.Slice(RowSelection::Indices(Span<const uint32_t>(s.rows.data(), w)),
              count);
    return OpResult::kNeedMoreInput;
  }

 private:
  struct State : OperatorState {
    FlexVector<uint32_t> rows;
  };
  uint32_t column_;
  const int64_t* key_;
};

// Runs a pipeline per key, rewinding one state as a cursor would.
void RunPipeline(benchmark::State& state,
                 const Source& pipeline,
                 int64_t* key,
                 uint32_t dur_column,
                 uint32_t key_bound) {
  std::unique_ptr<OperatorState> run = pipeline.MakeState();
  std::vector<int64_t> keys = Keys(4096, key_bound);
  RowBatch batch;
  uint32_t i = 0;
  int64_t rows = 0;
  for (auto _ : state) {
    *key = keys[i++ % keys.size()];
    pipeline.Rewind(*run);
    int64_t sum = 0;
    while (pipeline.GetData(batch, *run)) {
      const ColumnView& dur = batch.column(dur_column);
      for (uint32_t row = 0; row < batch.size(); ++row) {
        sum += dur.Value<int64_t>(row);
      }
      rows += batch.size();
    }
    benchmark::DoNotOptimize(sum);
  }
  state.counters["rows/query"] =
      static_cast<double>(rows) / static_cast<double>(state.iterations());
}

void BM_IdLookup_Interpreter(benchmark::State& state) {
  RunInterpreter(state, kId, false, kRows);
}
void BM_IdLookup_Pipeline(benchmark::State& state) {
  Table& t = GetTable();
  int64_t key = 0;
  IdSource source(t.dur, &key);
  Pipeline pipeline(source, {}, {});
  RunPipeline(state, pipeline, &key, 0, kRows);
}

// The source alone, without the Pipeline around it.
void BM_IdLookup_SourceOnly(benchmark::State& state) {
  Table& t = GetTable();
  int64_t key = 0;
  IdSource source(t.dur, &key);
  RunPipeline(state, source, &key, 0, kRows);
}

void BM_SortedEq_Interpreter(benchmark::State& state) {
  RunInterpreter(state, kUtid, false, kRows / kUtidRun);
}
void BM_SortedEq_Pipeline(benchmark::State& state) {
  Table& t = GetTable();
  int64_t key = 0;
  SortedEqSource source(t.utid, t.dur, &key);
  Pipeline pipeline(source, {}, {});
  RunPipeline(state, pipeline, &key, 0, kRows / kUtidRun);
}

void BM_FilterSort_Interpreter(benchmark::State& state) {
  RunInterpreter(state, kTrack, true, kTracks);
}
template <bool kFast>
void BM_FilterSort_Pipeline(benchmark::State& state) {
  Table& t = GetTable();
  int64_t key = 0;
  ScanSource source(t.track, t.dur);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<EqFilter<kFast>>(0, &key));
  SortSpec sort;
  sort.keys.push_back({1, false});
  ops.push_back(std::make_unique<Sort>(std::move(sort)));
  Pipeline pipeline(source, std::move(ops), {});
  RunPipeline(state, pipeline, &key, 1, kTracks);
}

BENCHMARK(BM_IdLookup_Interpreter);
BENCHMARK(BM_IdLookup_Pipeline);
BENCHMARK(BM_IdLookup_SourceOnly);
BENCHMARK(BM_SortedEq_Interpreter);
BENCHMARK(BM_SortedEq_Pipeline);
BENCHMARK(BM_FilterSort_Interpreter);
void BM_FilterOnly_Interpreter(benchmark::State& state) {
  RunInterpreter(state, kTrack, false, kTracks);
}
void BM_FilterOnly_Pipeline(benchmark::State& state) {
  Table& t = GetTable();
  int64_t key = 0;
  ScanSource source(t.track, t.dur);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<EqFilter<true>>(0, &key));
  Pipeline pipeline(source, std::move(ops), {});
  RunPipeline(state, pipeline, &key, 1, kTracks);
}
void BM_ScanOnly_Pipeline(benchmark::State& state) {
  Table& t = GetTable();
  int64_t key = 0;
  ScanSource source(t.track, t.dur);
  Pipeline pipeline(source, {}, {});
  RunPipeline(state, pipeline, &key, 1, kTracks);
}
BENCHMARK(BM_FilterOnly_Interpreter);
BENCHMARK(BM_FilterOnly_Pipeline);
BENCHMARK(BM_ScanOnly_Pipeline);
BENCHMARK_TEMPLATE(BM_FilterSort_Pipeline, false);
BENCHMARK_TEMPLATE(BM_FilterSort_Pipeline, true);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
