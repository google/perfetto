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

#include "src/trace_processor/core/exec/filter.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Shaped like a trace's slices, with none of the filtered columns sorted, so
// both the executor and the interpreter test every row.
inline constexpr auto kSlices = dataframe::CreateTypedDataframeSpec(
    {"id", "dur", "cpu", "depth", "name"},
    dataframe::CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}),
    dataframe::CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(Uint32{}, NonNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(Uint32{}, DenseNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}));

constexpr uint32_t kRows = 1u << 20;
constexpr uint32_t kCpus = 8;
constexpr uint32_t kNames = 64;

enum Column : uint32_t { kId, kDur, kCpu, kDepth, kName };

class Slices {
 public:
  Slices() : df_(dataframe::Dataframe::CreateFromTypedSpec(kSlices, &pool_)) {
    std::vector<StringPool::Id> names;
    for (uint32_t i = 0; i < kNames; ++i) {
      names.push_back(pool_.InternString(("name" + std::to_string(i)).c_str()));
    }
    for (uint32_t i = 0; i < kRows; ++i) {
      // A multiplicative hash spreads the values over the rows.
      uint32_t hash = i * 2654435761u;
      std::optional<uint32_t> depth;
      if (hash % 5 != 0) {
        depth = (hash >> 4) % 16;
      }
      df_.InsertUnchecked(kSlices, std::monostate{},
                          int64_t{(hash >> 12) % 1000}, (hash >> 8) % kCpus,
                          depth, names[(hash >> 16) % kNames]);
    }
    df_.Finalize();
  }

  const dataframe::Dataframe& df() const { return df_; }
  const StringPool& pool() const { return pool_; }

 private:
  StringPool pool_;
  dataframe::Dataframe df_;
};

const Slices& GetSlices() {
  static const Slices* slices = new Slices();
  return *slices;
}

using Value = std::variant<int64_t, std::string>;

struct OneValue : ValueFetcher {
  using Type = size_t;
  static constexpr Type kInt64 = base::variant_index<Value, int64_t>();
  static constexpr Type kString = base::variant_index<Value, std::string>();
  static constexpr Type kDouble = kString + 1;
  static constexpr Type kNull = kString + 2;

  explicit OneValue(const Value& v) : value(v) {}

  int64_t GetInt64Value(uint32_t) const { return std::get<int64_t>(value); }
  double GetDoubleValue(uint32_t) const { PERFETTO_FATAL("No doubles"); }
  const char* GetStringValue(uint32_t) const {
    return std::get<std::string>(value).c_str();
  }
  Type GetValueType(uint32_t) const { return value.index(); }
  bool IteratorInit(uint32_t) { return false; }
  bool IteratorNext(uint32_t) { return false; }

  const Value& value;
};

// Scans every row and filters each batch, the filter's value cast as each run
// starts.
void RunExecutor(benchmark::State& state,
                 uint32_t column,
                 Op op,
                 const Value& value) {
  const Slices& s = GetSlices();
  const dataframe::Dataframe& df = s.df();
  Filter filter({{1, op, 0}}, &s.pool());
  DataframeScan scan({df.shared_column(kId), df.shared_column(column)},
                     df.row_count());
  FilterValueCaster<OneValue> caster =
      GetFilterValueCaster<OneValue>(df.column_type(column), op);
  OneValue fetcher(value);
  FilterValue cast[1];

  Context context;
  context.SetFilterValues(Span<const FilterValue>(cast));
  std::unique_ptr<OperatorState> scan_state = scan.MakeState(context);
  std::unique_ptr<OperatorState> filter_state = filter.MakeState(context);
  RowBatch batch;
  for (auto _ : state) {
    caster(fetcher, 0, s.pool(), &cast[0]);
    scan.Rewind(*scan_state);
    uint32_t rows = 0;
    while (scan.GetData(batch, *scan_state)) {
      filter.Process(batch, *filter_state);
      rows += batch.size();
    }
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRows);
}

// The same filter, run by the dataframe interpreter.
void RunInterpreter(benchmark::State& state,
                    uint32_t column,
                    Op op,
                    const Value& value) {
  const Slices& s = GetSlices();
  const dataframe::Dataframe& df = s.df();
  std::vector<dataframe::FilterSpec> specs = {{column, 0, op, std::nullopt}};
  auto plan = df.PlanQuery(specs, {}, {}, {}, uint64_t{1} << kId);
  PERFETTO_CHECK(plan.ok());
  dataframe::Cursor<OneValue> cursor;
  df.PrepareCursor(*plan, cursor);
  OneValue fetcher(value);
  for (auto _ : state) {
    cursor.Execute(fetcher);
    uint32_t rows = 0;
    for (; !cursor.Eof(); cursor.Next()) {
      ++rows;
    }
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRows);
}

// An eighth of the rows, spread throughout.
void BM_ExecFilterEqUint32(benchmark::State& state) {
  RunExecutor(state, kCpu, Eq{}, int64_t{3});
}
BENCHMARK(BM_ExecFilterEqUint32);
void BM_InterpreterFilterEqUint32(benchmark::State& state) {
  RunInterpreter(state, kCpu, Eq{}, int64_t{3});
}
BENCHMARK(BM_InterpreterFilterEqUint32);

// Half the rows.
void BM_ExecFilterLtInt64(benchmark::State& state) {
  RunExecutor(state, kDur, Lt{}, int64_t{500});
}
BENCHMARK(BM_ExecFilterLtInt64);
void BM_InterpreterFilterLtInt64(benchmark::State& state) {
  RunInterpreter(state, kDur, Lt{}, int64_t{500});
}
BENCHMARK(BM_InterpreterFilterLtInt64);

// A sixteenth of the rows which are not null.
void BM_ExecFilterEqNullable(benchmark::State& state) {
  RunExecutor(state, kDepth, Eq{}, int64_t{3});
}
BENCHMARK(BM_ExecFilterEqNullable);
void BM_InterpreterFilterEqNullable(benchmark::State& state) {
  RunInterpreter(state, kDepth, Eq{}, int64_t{3});
}
BENCHMARK(BM_InterpreterFilterEqNullable);

// A sixty-fourth of the rows, by string.
void BM_ExecFilterEqString(benchmark::State& state) {
  RunExecutor(state, kName, Eq{}, std::string("name7"));
}
BENCHMARK(BM_ExecFilterEqString);
void BM_InterpreterFilterEqString(benchmark::State& state) {
  RunInterpreter(state, kName, Eq{}, std::string("name7"));
}
BENCHMARK(BM_InterpreterFilterEqString);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
