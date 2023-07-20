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
#include <initializer_list>
#include <string>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace {

using SliceTable = tables::SliceTable;
using ThreadTrackTable = tables::ThreadTrackTable;
using ExpectedFrameTimelineSliceTable = tables::ExpectedFrameTimelineSliceTable;
using RawTable = tables::RawTable;
using FtraceEventTable = tables::FtraceEventTable;

// `SELECT * FROM SLICE` on android_monitor_contention_trace.at
static char kSliceTable[] = "test/data/slice_table_for_benchmarks.csv";

// `SELECT * FROM SLICE` on android_monitor_contention_trace.at
static char kExpectedFrameTimelineTable[] =
    "test/data/expected_frame_timeline_for_benchmarks.csv";

// `SELECT id, cpu FROM raw` on chrome_android_systrace.pftrace.
static char kRawTable[] = "test/data/raw_cpu_for_benchmarks.csv";

// `SELECT id, cpu FROM ftrace_event` on chrome_android_systrace.pftrace.
static char kFtraceEventTable[] =
    "test/data/ftrace_event_cpu_for_benchmarks.csv";

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

std::vector<std::string> ReadCSV(benchmark::State& state,
                                 std::string file_name) {
  std::string table_csv;
  perfetto::base::ReadFile(perfetto::base::GetTestDataPath(file_name),
                           &table_csv);
  if (table_csv.empty()) {
    state.SkipWithError(
        "Test strings missing. Googlers: download "
        "go/perfetto-benchmark-trace-strings and save into /tmp/trace_strings");
    return {};
  }
  PERFETTO_CHECK(!table_csv.empty());
  return base::SplitString(table_csv, "\n");
}

SliceTable::Row GetSliceTableRow(const std::string& string_row,
                                 StringPool& pool) {
  std::vector<std::string> row_vec = SplitCSVLine(string_row);
  SliceTable::Row row;
  PERFETTO_CHECK(row_vec.size() >= 12);
  row.ts = *base::StringToInt64(row_vec[2]);
  row.dur = *base::StringToInt64(row_vec[3]);
  row.track_id = ThreadTrackTable::Id(*base::StringToUInt32(row_vec[4]));
  row.category = pool.InternString(base::StringView(row_vec[5]));
  row.name = pool.InternString(base::StringView(row_vec[6]));
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
  return row;
}

struct SliceTableForBenchmark {
  explicit SliceTableForBenchmark(benchmark::State& state) : table_{&pool_} {
    std::vector<std::string> rows_strings = ReadCSV(state, kSliceTable);

    for (size_t i = 1; i < rows_strings.size(); ++i) {
      table_.Insert(GetSliceTableRow(rows_strings[i], pool_));
    }
  }

  StringPool pool_;
  SliceTable table_;
};

struct ExpectedFrameTimelineTableForBenchmark {
  explicit ExpectedFrameTimelineTableForBenchmark(benchmark::State& state)
      : table_{&pool_, &parent_} {
    std::vector<std::string> table_rows_as_string =
        ReadCSV(state, kExpectedFrameTimelineTable);
    std::vector<std::string> parent_rows_as_string =
        ReadCSV(state, kSliceTable);

    uint32_t cur_idx = 0;
    for (size_t i = 1; i < table_rows_as_string.size(); ++i, ++cur_idx) {
      std::vector<std::string> row_vec = SplitCSVLine(table_rows_as_string[i]);

      uint32_t idx = *base::StringToUInt32(row_vec[0]);
      while (cur_idx < idx) {
        parent_.Insert(
            GetSliceTableRow(parent_rows_as_string[cur_idx + 1], pool_));
        cur_idx++;
      }

      ExpectedFrameTimelineSliceTable::Row row;
      row.ts = *base::StringToInt64(row_vec[2]);
      row.dur = *base::StringToInt64(row_vec[3]);
      row.track_id = ThreadTrackTable::Id(*base::StringToUInt32(row_vec[4]));
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
      table_.Insert(row);
    }
  }
  StringPool pool_;
  SliceTable parent_{&pool_};
  ExpectedFrameTimelineSliceTable table_;
};

struct FtraceEventTableForBenchmark {
  explicit FtraceEventTableForBenchmark(benchmark::State& state) {
    std::vector<std::string> raw_rows = ReadCSV(state, kRawTable);
    std::vector<std::string> ftrace_event_rows =
        ReadCSV(state, kFtraceEventTable);

    uint32_t cur_idx = 0;
    for (size_t i = 1; i < ftrace_event_rows.size(); ++i, cur_idx++) {
      std::vector<std::string> row_vec = SplitCSVLine(ftrace_event_rows[i]);
      uint32_t idx = *base::StringToUInt32(row_vec[0]);
      while (cur_idx < idx) {
        std::vector<std::string> raw_row = SplitCSVLine(raw_rows[cur_idx + 1]);
        RawTable::Row r;
        r.cpu = *base::StringToUInt32(raw_row[1]);
        raw_.Insert(r);
        cur_idx++;
      }
      FtraceEventTable::Row row;
      row.cpu = *base::StringToUInt32(row_vec[1]);
      table_.Insert(row);
    }
  }

  StringPool pool_;
  RawTable raw_{&pool_};
  tables::FtraceEventTable table_{&pool_, &raw_};
};

void BenchmarkSliceTable(benchmark::State& state,
                         SliceTableForBenchmark& table,
                         std::initializer_list<Constraint> c) {
  Table::kUseFilterV2 = state.range(0) == 1;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToRowMap(c));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

void BenchmarkExpectedFrameTable(benchmark::State& state,
                                 ExpectedFrameTimelineTableForBenchmark& table,
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

void BenchmarkFtraceEventTable(benchmark::State& state,
                               FtraceEventTableForBenchmark& table,
                               std::initializer_list<Constraint> c) {
  Table::kUseFilterV2 = state.range(0) == 1;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToRowMap(c));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

static void BM_QESliceTableTrackIdEq(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.track_id().eq(100)});
}

BENCHMARK(BM_QESliceTableTrackIdEq)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableParentIdIsNotNull(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.parent_id().is_not_null()});
}

BENCHMARK(BM_QESliceTableParentIdIsNotNull)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableParentIdEq(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.parent_id().eq(88)});
}

BENCHMARK(BM_QESliceTableParentIdEq)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableNameEq(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.name().eq("cheese")});
}

BENCHMARK(BM_QESliceTableNameEq)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableNameGlobNoStars(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.name().glob("cheese")});
}

BENCHMARK(BM_QESliceTableNameGlobNoStars)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableNameGlob(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.name().glob("chee*se")});
}

BENCHMARK(BM_QESliceTableNameGlob)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableNameRegex(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.name().regex(".*Pool.*")});
}

BENCHMARK(BM_QESliceTableNameRegex)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableSorted(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.ts().gt(1000)});
}

BENCHMARK(BM_QESliceTableSorted)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QEFilterWithSparseSelector(benchmark::State& state) {
  ExpectedFrameTimelineTableForBenchmark table(state);
  BenchmarkExpectedFrameTable(state, table, table.table_.track_id().eq(88));
}

BENCHMARK(BM_QEFilterWithSparseSelector)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QEFilterWithDenseSelector(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  BenchmarkFtraceEventTable(state, table, {table.table_.cpu().eq(4)});
}

BENCHMARK(BM_QEFilterWithDenseSelector)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceEventFilterId(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(state, table, {table.table_.id().eq(500)});
}

BENCHMARK(BM_QESliceEventFilterId)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QEFtraceEventFilterId(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  BenchmarkFtraceEventTable(state, table, {table.table_.id().eq(500)});
}

BENCHMARK(BM_QEFtraceEventFilterId)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QESliceTableTsAndTrackId(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTable(
      state, table,
      {table.table_.ts().ge(1740530419866), table.table_.ts().le(1740530474097),
       table.table_.track_id().eq(100)});
}

BENCHMARK(BM_QESliceTableTsAndTrackId)->ArgsProduct({{DB::V1, DB::V2}});

static void BM_QEFilterWithArrangement(benchmark::State& state) {
  Table::kUseFilterV2 = state.range(0) == 1;

  SliceTableForBenchmark table(state);
  Order order{table.table_.dur().index_in_table(), false};
  Table slice_sorted_with_duration = table.table_.Sort({order});

  Constraint c{table.table_.track_id().index_in_table(), FilterOp::kGt,
               SqlValue::Long(10)};
  for (auto _ : state) {
    benchmark::DoNotOptimize(slice_sorted_with_duration.FilterToRowMap({c}));
  }
  state.counters["s/row"] = benchmark::Counter(
      static_cast<double>(slice_sorted_with_duration.row_count()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
}

BENCHMARK(BM_QEFilterWithArrangement)->ArgsProduct({{DB::V1, DB::V2}});

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
