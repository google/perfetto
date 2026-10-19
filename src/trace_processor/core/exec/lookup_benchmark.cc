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

// Lookups of a few rows of a big table, run once per row of another: what
// joins on an id do millions of times. Each executor case is timed against
// the interpreter running the same query through TypedCursor, which is how
// C++ callers run it today.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/typed_cursor.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/sorted_scan.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using Fetcher = dataframe::TypedCursor::Fetcher;
using FetcherValue = dataframe::TypedCursor::FilterValue;

constexpr uint32_t kRows = 1u << 20;
// Lookups cycle through up to this many random keys: too many for the rows
// they find to stay in cache. Each lookup benchmark also runs with 64, which
// stay in cache, leaving only what each run costs the CPU.
constexpr uint32_t kLookups = 1u << 16;

// -----------------------------------------------------------------------------
// Tables
// -----------------------------------------------------------------------------

// Shaped like the slice table: an id, ts ascending and a dur.
enum SliceCol : uint32_t { kTs, kDur, kSliceId };

struct Slices {
  Slices() {
    std::minstd_rand rng(2);
    dataframe::AdhocDataframeBuilder builder({"ts", "dur"}, &pool);
    int64_t ts = int64_t{1} << 40;
    for (uint32_t i = 0; i < kRows; ++i) {
      ts += 1 + rng() % 100;
      PERFETTO_CHECK(builder.PushNonNull(kTs, ts));
      PERFETTO_CHECK(builder.PushNonNull(
          kDur, (int64_t{1} << 40) + int64_t(rng() % 1000)));
      all_ts.push_back(ts);
    }
    auto built = std::move(builder).Build();
    PERFETTO_CHECK(built.ok());
    df = std::make_unique<dataframe::Dataframe>(std::move(*built));
    PERFETTO_CHECK(df->column(kTs).sort_state.Is<Sorted>());
    PERFETTO_CHECK(df->column(kSliceId).storage.type().Is<Id>());
    for (uint32_t i = 0; i < kLookups; ++i) {
      rows.push_back(static_cast<uint32_t>(rng() % kRows));
    }
  }

  StringPool pool;
  std::unique_ptr<dataframe::Dataframe> df;
  std::vector<int64_t> all_ts;
  std::vector<uint32_t> rows;
};

const Slices& GetSlices() {
  static const Slices* slices = new Slices();
  return *slices;
}

// -----------------------------------------------------------------------------
// Running a plan
// -----------------------------------------------------------------------------

// A plan run as a C++ caller would run it on the executor: with the values a
// TypedCursor would fetch from, cast for each condition by a caster chosen
// once, then read through a RowCursor.
class ExecLookup {
 public:
  struct Condition {
    StorageType type;
    Op op;
  };

  ExecLookup(std::unique_ptr<Source> scan,
             std::vector<Pipeline::Step> steps,
             const std::vector<Condition>& conditions,
             const StringPool* pool)
      : scan_(std::move(scan)),
        pipeline_(*scan_, std::move(steps)),
        // A plan of a source alone is read straight from the source, as
        // lowering it would.
        cursor_(pipeline_.steps().empty()
                    ? *scan_
                    : static_cast<const Source&>(pipeline_)),
        values_(conditions.size()),
        cast_(conditions.size()),
        pool_(pool) {
    for (const Condition& c : conditions) {
      casters_.push_back(GetFilterValueCaster<Fetcher>(c.type, c.op));
    }
    fetcher_.filter_values_ = values_.data();
  }

  void Set(uint32_t i, FetcherValue v) { values_[i] = v; }

  // Runs the plan, returning whether it found a row.
  PERFETTO_ALWAYS_INLINE bool Open() {
    for (uint32_t i = 0; i < casters_.size(); ++i) {
      casters_[i](fetcher_, i, *pool_, &cast_[i]);
    }
    return cursor_.Open(
        Span<const FilterValue>(cast_.data(), cast_.data() + cast_.size()));
  }
  RowCursor& cursor() { return cursor_; }

 private:
  std::unique_ptr<Source> scan_;
  Pipeline pipeline_;
  RowCursor cursor_;
  std::vector<FetcherValue> values_;
  std::vector<FilterValue> cast_;
  std::vector<FilterValueCaster<Fetcher>> casters_;
  Fetcher fetcher_;
  const StringPool* pool_;
};

std::vector<std::shared_ptr<const dataframe::Column>> Columns(
    const dataframe::Dataframe& df,
    std::initializer_list<uint32_t> cols) {
  std::vector<std::shared_ptr<const dataframe::Column>> out;
  for (uint32_t c : cols) {
    out.push_back(df.shared_column(c));
  }
  return out;
}

// -----------------------------------------------------------------------------
// id = ?
// -----------------------------------------------------------------------------

void BM_IdEq_Interpreter(benchmark::State& state) {
  const Slices& s = GetSlices();
  dataframe::TypedCursor cursor(s.df.get(), {{kSliceId, 0, Eq{}, std::nullopt}},
                                {});
  uint32_t i = 0;
  auto n = static_cast<uint32_t>(state.range(0));
  uint32_t found = 0;
  for (auto _ : state) {
    cursor.SetFilterValueUnchecked(0, s.rows[i++ % n]);
    cursor.ExecuteUnchecked();
    found += !cursor.Eof() && cursor.RowIndex() != 0;
  }
  benchmark::DoNotOptimize(found);
}
BENCHMARK(BM_IdEq_Interpreter)->Arg(64)->Arg(kLookups);

void RunIdEq(benchmark::State& state, std::unique_ptr<Source> scan) {
  const Slices& s = GetSlices();
  ExecLookup lookup(std::move(scan), {}, {{Id{}, Eq{}}}, &s.pool);
  uint32_t i = 0;
  auto n = static_cast<uint32_t>(state.range(0));
  uint32_t found = 0;
  for (auto _ : state) {
    lookup.Set(0, int64_t{s.rows[i++ % n]});
    found += lookup.Open() && lookup.cursor().Value<uint32_t>(0) != 0;
  }
  benchmark::DoNotOptimize(found);
}

void BM_IdEq_SortedScan(benchmark::State& state) {
  const Slices& s = GetSlices();
  RunIdEq(state, std::make_unique<SortedScan>(
                     Columns(*s.df, {kSliceId}), s.df->row_count(),
                     std::vector<SortedScan::Search>{
                         {s.df->shared_column(kSliceId),
                          SortedScan::Search::Kind::kBinarySearch, Eq{}, 0}},
                     &s.pool));
}
BENCHMARK(BM_IdEq_SortedScan)->Arg(64)->Arg(kLookups);

// -----------------------------------------------------------------------------
// ts = ?, a binary search
// -----------------------------------------------------------------------------

void BM_TsEq_Interpreter(benchmark::State& state) {
  const Slices& s = GetSlices();
  dataframe::TypedCursor cursor(s.df.get(), {{kTs, 0, Eq{}, std::nullopt}}, {});
  uint32_t i = 0;
  auto n = static_cast<uint32_t>(state.range(0));
  uint32_t found = 0;
  for (auto _ : state) {
    cursor.SetFilterValueUnchecked(0, s.all_ts[s.rows[i++ % n]]);
    cursor.ExecuteUnchecked();
    found += !cursor.Eof() && cursor.RowIndex() != 0;
  }
  benchmark::DoNotOptimize(found);
}
BENCHMARK(BM_TsEq_Interpreter)->Arg(64)->Arg(kLookups);

void BM_TsEq_SortedScan(benchmark::State& state) {
  const Slices& s = GetSlices();
  ExecLookup lookup(std::make_unique<SortedScan>(
                        Columns(*s.df, {kSliceId}), s.df->row_count(),
                        std::vector<SortedScan::Search>{
                            {s.df->shared_column(kTs),
                             SortedScan::Search::Kind::kBinarySearch, Eq{}, 0}},
                        &s.pool),
                    {}, {{Int64{}, Eq{}}}, &s.pool);
  uint32_t i = 0;
  auto n = static_cast<uint32_t>(state.range(0));
  uint32_t found = 0;
  for (auto _ : state) {
    lookup.Set(0, s.all_ts[s.rows[i++ % n]]);
    found += lookup.Open() && lookup.cursor().Value<uint32_t>(0) != 0;
  }
  benchmark::DoNotOptimize(found);
}
BENCHMARK(BM_TsEq_SortedScan)->Arg(64)->Arg(kLookups);

// -----------------------------------------------------------------------------
// ts >= ? AND ts < ?, reading dur for every row
// -----------------------------------------------------------------------------

// The arg is roughly how many rows each range holds.
void BM_TsRange_Interpreter(benchmark::State& state) {
  const Slices& s = GetSlices();
  auto span = static_cast<uint32_t>(state.range(0));
  dataframe::TypedCursor cursor(
      s.df.get(), {{kTs, 0, Ge{}, std::nullopt}, {kTs, 1, Lt{}, std::nullopt}},
      {});
  uint32_t i = 0;
  int64_t sum = 0;
  int64_t rows = 0;
  const auto& dur = s.df->column(kDur).storage.unchecked_get<Int64>();
  for (auto _ : state) {
    uint32_t r = s.rows[i++ % kLookups] % (kRows - span);
    cursor.SetFilterValueUnchecked(0, s.all_ts[r]);
    cursor.SetFilterValueUnchecked(1, s.all_ts[r + span]);
    for (cursor.ExecuteUnchecked(); !cursor.Eof(); cursor.Next()) {
      // How a typed C++ caller reads a cell: through the row index.
      sum += dur[cursor.RowIndex()];
      ++rows;
    }
  }
  benchmark::DoNotOptimize(sum);
  state.SetItemsProcessed(rows);
}
BENCHMARK(BM_TsRange_Interpreter)->Arg(16)->Arg(1024)->Arg(65536);

void BM_TsRange_SortedScan(benchmark::State& state) {
  const Slices& s = GetSlices();
  auto span = static_cast<uint32_t>(state.range(0));
  ExecLookup lookup(std::make_unique<SortedScan>(
                        Columns(*s.df, {kDur}), s.df->row_count(),
                        std::vector<SortedScan::Search>{
                            {s.df->shared_column(kTs),
                             SortedScan::Search::Kind::kBinarySearch, Ge{}, 0},
                            {s.df->shared_column(kTs),
                             SortedScan::Search::Kind::kBinarySearch, Lt{}, 1}},
                        &s.pool),
                    {}, {{Int64{}, Ge{}}, {Int64{}, Lt{}}}, &s.pool);
  uint32_t i = 0;
  int64_t sum = 0;
  int64_t rows = 0;
  for (auto _ : state) {
    uint32_t r = s.rows[i++ % kLookups] % (kRows - span);
    lookup.Set(0, s.all_ts[r]);
    lookup.Set(1, s.all_ts[r + span]);
    for (bool ok = lookup.Open(); ok; ok = lookup.cursor().Next()) {
      sum += lookup.cursor().Value<int64_t>(0);
      ++rows;
    }
  }
  benchmark::DoNotOptimize(sum);
  state.SetItemsProcessed(rows);
}
BENCHMARK(BM_TsRange_SortedScan)->Arg(16)->Arg(1024)->Arg(65536);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
