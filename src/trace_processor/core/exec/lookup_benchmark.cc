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

// Searches of a big table, run once per row of another: what extract_arg and
// time range queries do millions of times. Each executor case is timed
// against the interpreter running the same query through TypedCursor, which
// is how C++ callers run it today.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/typed_cursor.h"
#include "src/trace_processor/core/exec/filter.h"
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
// Few enough that the rows found stay in cache, leaving what each run costs
// the CPU.
constexpr uint32_t kLookups = 64;

// Shaped like the args table: arg sets of 1 to 20 rows, each a key and a
// value, with the sets numbered by their first row.
enum ArgCol : uint32_t { kArgSetId, kKey, kIntValue, kArgId };

struct Args {
  Args() {
    std::minstd_rand rng(1);
    std::vector<StringPool::Id> keys;
    for (uint32_t i = 0; i < 200; ++i) {
      keys.push_back(pool.InternString(
          base::StackString<32>("debug.key_%u", i).string_view()));
    }
    dataframe::AdhocDataframeBuilder builder({"arg_set_id", "key", "int_value"},
                                             &pool);
    uint32_t row = 0;
    while (row < kRows) {
      uint32_t size =
          std::min(1 + static_cast<uint32_t>(rng() % 20), kRows - row);
      // The keys of a set are distinct.
      uint32_t first_key = static_cast<uint32_t>(rng() % keys.size());
      for (uint32_t i = 0; i < size; ++i) {
        PERFETTO_CHECK(builder.PushNonNull(kArgSetId, int64_t{row}));
        PERFETTO_CHECK(
            builder.PushNonNull(kKey, keys[(first_key + i) % keys.size()]));
        if (rng() % 3 == 0) {
          builder.PushNull(kIntValue);
        } else {
          PERFETTO_CHECK(builder.PushNonNull(
              kIntValue, (int64_t{1} << 40) + int64_t(rng() % 1000)));
        }
      }
      row += size;
    }
    auto built = std::move(builder).Build();
    PERFETTO_CHECK(built.ok());
    df = std::make_unique<dataframe::Dataframe>(std::move(*built));
    PERFETTO_CHECK(df->column(kArgSetId).sort_state.Is<SetIdSorted>());

    // Each lookup is of a key its set has.
    struct Key : dataframe::CellCallback {
      void OnCell(int64_t) {}
      void OnCell(double) {}
      void OnCell(NullTermStringView v) { out = v.c_str(); }
      void OnCell(std::nullptr_t) {}
      void OnCell(uint32_t v) { set = v; }
      void OnCell(int32_t) {}
      uint32_t set = 0;
      const char* out = nullptr;
    };
    for (uint32_t i = 0; i < kLookups; ++i) {
      uint32_t r = static_cast<uint32_t>(rng() % kRows);
      Key k;
      df->GetCell(r, kArgSetId, k);
      df->GetCell(r, kKey, k);
      lookups.push_back({k.set, k.out});
    }
  }

  struct Lookup {
    uint32_t set;
    const char* key;
  };
  StringPool pool;
  std::unique_ptr<dataframe::Dataframe> df;
  std::vector<Lookup> lookups;
};

const Args& GetArgs() {
  static const Args* args = new Args();
  return *args;
}

// Shaped like the slice table: ts ascending and a dur.
enum SliceCol : uint32_t { kTs, kDur };

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

// Runs a plan on the executor the way a C++ caller would.
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
        // A lone source is read directly, as lowering would.
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

// extract_arg: arg_set_id = ? AND key = ?

void BM_ExtractArg_Interpreter(benchmark::State& state) {
  const Args& a = GetArgs();
  dataframe::TypedCursor cursor(
      a.df.get(),
      {{kArgSetId, 0, Eq{}, std::nullopt}, {kKey, 1, Eq{}, std::nullopt}}, {});
  uint32_t i = 0;
  uint32_t found = 0;
  for (auto _ : state) {
    const Args::Lookup& l = a.lookups[i++ % kLookups];
    cursor.SetFilterValueUnchecked(0, l.set);
    cursor.SetFilterValueUnchecked(1, l.key);
    cursor.ExecuteUnchecked();
    if (!cursor.Eof()) {
      found += cursor.RowIndex() != 0;
    }
  }
  benchmark::DoNotOptimize(found);
}
BENCHMARK(BM_ExtractArg_Interpreter);

// Runs `scan` reading the arg id and key, with the key filtered after.
void RunExtractArg(benchmark::State& state, std::unique_ptr<Source> scan) {
  const Args& a = GetArgs();
  std::vector<Pipeline::Step> steps;
  steps.emplace_back(std::make_unique<Filter>(
      std::vector<Filter::Condition>{{1, Eq{}, 1}}, &a.pool));
  ExecLookup lookup(std::move(scan), std::move(steps),
                    {{Uint32{}, Eq{}}, {String{}, Eq{}}}, &a.pool);
  uint32_t i = 0;
  uint32_t found = 0;
  for (auto _ : state) {
    const Args::Lookup& l = a.lookups[i++ % kLookups];
    lookup.Set(0, int64_t{l.set});
    lookup.Set(1, l.key);
    if (lookup.Open()) {
      found += lookup.cursor().Value<uint32_t>(0) != 0;
    }
  }
  benchmark::DoNotOptimize(found);
}

void BM_ExtractArg_SortedScan(benchmark::State& state) {
  const Args& a = GetArgs();
  RunExtractArg(state, std::make_unique<SortedScan>(
                           Columns(*a.df, {kArgId, kKey}), a.df->row_count(),
                           std::vector<SortedScan::Search>{
                               {a.df->shared_column(kArgSetId),
                                SortedScan::Search::Kind::kSetIdEq, Eq{}, 0}},
                           &a.pool));
}
BENCHMARK(BM_ExtractArg_SortedScan);

// ts >= ? AND ts < ?, reading dur for every row. The arg is roughly how many
// rows each range holds.

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
BENCHMARK(BM_TsRange_Interpreter)->Arg(16)->Arg(65536);

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
BENCHMARK(BM_TsRange_SortedScan)->Arg(16)->Arg(65536);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
