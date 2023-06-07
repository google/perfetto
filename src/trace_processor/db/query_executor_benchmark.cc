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

#include <benchmark/benchmark.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/tables/slice_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace {

using SliceTable = tables::SliceTable;

enum DB { V1, V2 };

std::vector<std::string> SplitCSVLine(const std::string& line) {
  std::vector<std::string> output;
  uint32_t start = 0;
  bool in_string = false;

  for (uint32_t i = 0; i < line.size(); ++i) {
    if (!in_string && line[i] == ',') {
      output.emplace_back(&line[start], i - start);
      start = i + 1;
      continue;
    }
    if (line[i] == '"')
      in_string = !in_string;
  }

  if (start < line.size())
    output.emplace_back(&line[start], line.size() - start);

  return output;
}

std::vector<SliceTable::Row> LoadRowsFromCSVToSliceTable(
    benchmark::State& state) {
  std::vector<SliceTable::Row> rows;
  std::string table_csv;
  static const char kTestTrace[] = "test/data/example_android_trace_30s.csv";
  perfetto::base::ReadFile(perfetto::base::GetTestDataPath(kTestTrace),
                           &table_csv);
  if (table_csv.empty()) {
    state.SkipWithError(
        "Test strings missing. Googlers: download "
        "go/perfetto-benchmark-trace-strings and save into /tmp/trace_strings");
    return {};
  }
  PERFETTO_CHECK(!table_csv.empty());

  std::vector<std::string> rows_strings = base::SplitString(table_csv, "\n");
  for (size_t i = 1; i < rows_strings.size(); ++i) {
    std::vector<std::string> row_vec = SplitCSVLine(rows_strings[i]);
    SliceTable::Row row;
    PERFETTO_CHECK(row_vec.size() >= 12);
    row.ts = *base::StringToInt64(row_vec[2]);
    row.dur = *base::StringToInt64(row_vec[3]);
    row.track_id =
        tables::ThreadTrackTable::Id(*base::StringToUInt32(row_vec[4]));
    row.depth = *base::StringToUInt32(row_vec[7]);
    row.stack_id = *base::StringToInt32(row_vec[8]);
    row.parent_stack_id = *base::StringToInt32(row_vec[9]);
    row.parent_id = base::StringToUInt32(row_vec[11]).has_value()
                        ? std::make_optional<SliceTable::Id>(
                              *base::StringToUInt32(row_vec[11]))
                        : std::nullopt;
    row.arg_set_id = *base::StringToUInt32(row_vec[11]);
    row.thread_ts = base::StringToInt64(row_vec[12]);
    row.thread_dur = base::StringToInt64(row_vec[13]);
    rows.emplace_back(row);
  }
  return rows;
}

struct BenchmarkSliceTable {
  explicit BenchmarkSliceTable(benchmark::State& state) : table_{&pool_} {
    auto rows = LoadRowsFromCSVToSliceTable(state);
    for (uint32_t i = 0; i < rows.size(); ++i) {
      table_.Insert(rows[i]);
    }
  }
  StringPool pool_;
  SliceTable table_;
};

void SliceTableBenchmark(benchmark::State& state,
                         BenchmarkSliceTable& table,
                         Constraint c) {
  Table::kUseFilterV2 = state.range(0) == 1;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToRowMap({c}));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

static void BM_DBv2SliceTableTrackIdEquals(benchmark::State& state) {
  BenchmarkSliceTable table(state);
  SliceTableBenchmark(state, table, table.table_.track_id().eq(100));
}

BENCHMARK(BM_DBv2SliceTableTrackIdEquals)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_DBv2SliceTableParentIdIsNotNull(benchmark::State& state) {
  BenchmarkSliceTable table(state);
  SliceTableBenchmark(state, table, table.table_.parent_id().is_not_null());
}

BENCHMARK(BM_DBv2SliceTableParentIdIsNotNull)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_DBv2SliceTableParentIdEq(benchmark::State& state) {
  BenchmarkSliceTable table(state);
  SliceTableBenchmark(state, table, table.table_.parent_id().eq(88));
}

BENCHMARK(BM_DBv2SliceTableParentIdEq)->ArgsProduct({{DB::V1, DB::V2}});

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
