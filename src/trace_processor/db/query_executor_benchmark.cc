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

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"

namespace perfetto::trace_processor {
namespace {

using SliceTable = tables::SliceTable;
using ThreadTrackTable = tables::ThreadTrackTable;
using ExpectedFrameTimelineSliceTable = tables::ExpectedFrameTimelineSliceTable;
using RawTable = tables::RawTable;
using FtraceEventTable = tables::FtraceEventTable;
using HeapGraphObjectTable = tables::HeapGraphObjectTable;

// `SELECT * FROM SLICE` on android_monitor_contention_trace.at
constexpr std::string_view kSliceTable =
    "test/data/slice_table_for_benchmarks.csv";

// `SELECT * FROM SLICE` on android_monitor_contention_trace.at
constexpr std::string_view kExpectedFrameTimelineTable =
    "test/data/expected_frame_timeline_for_benchmarks.csv";

// `SELECT id, cpu FROM raw` on chrome_android_systrace.pftrace.
constexpr std::string_view kRawTable = "test/data/raw_cpu_for_benchmarks.csv";

// `SELECT id, cpu FROM ftrace_event` on chrome_android_systrace.pftrace.
constexpr std::string_view kFtraceEventTable =
    "test/data/ftrace_event_cpu_for_benchmarks.csv";

// `SELECT id, upid, reference_set_id FROM heap_graph_object` on
constexpr std::string_view kHeapGraphObjectTable =
    "test/data/heap_pgraph_object_for_benchmarks_query.csv";

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
                                 std::string_view file_name) {
  std::string table_csv;
  perfetto::base::ReadFile(
      perfetto::base::GetTestDataPath(std::string(file_name)), &table_csv);
  if (table_csv.empty()) {
    state.SkipWithError(
        "Test strings missing. Googlers: download "
        "go/perfetto-benchmark-trace-strings and save into /tmp/trace_strings");
    return {};
  }
  PERFETTO_CHECK(!table_csv.empty());
  return base::SplitString(table_csv, "\n");
}

template <typename It>
double CountRows(It it) {
  double i = 0;
  for (; it; ++it, ++i) {
  }
  return i;
}

StringPool::Id StripAndIntern(StringPool& pool, const std::string& data) {
  std::string res = base::StripSuffix(base::StripPrefix(data, "\""), "\"");
  return pool.InternString(base::StringView(res));
}

SliceTable::Row GetSliceTableRow(const std::string& string_row,
                                 StringPool& pool) {
  std::vector<std::string> row_vec = SplitCSVLine(string_row);
  SliceTable::Row row;
  PERFETTO_CHECK(row_vec.size() >= 14);
  row.ts = *base::StringToInt64(row_vec[2]);
  row.dur = *base::StringToInt64(row_vec[3]);
  row.track_id = ThreadTrackTable::Id(*base::StringToUInt32(row_vec[4]));
  row.category = StripAndIntern(pool, row_vec[5]);
  row.name = StripAndIntern(pool, row_vec[6]);
  row.depth = *base::StringToUInt32(row_vec[7]);
  row.stack_id = *base::StringToInt32(row_vec[8]);
  row.parent_stack_id = *base::StringToInt32(row_vec[9]);
  row.parent_id = base::StringToUInt32(row_vec[10]).has_value()
                      ? std::make_optional<SliceTable::Id>(
                            *base::StringToUInt32(row_vec[10]))
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
        r.ucpu = tables::CpuTable::Id(*base::StringToUInt32(raw_row[1]));
        raw_.Insert(r);
        cur_idx++;
      }
      FtraceEventTable::Row row;
      row.ucpu = tables::CpuTable::Id(*base::StringToUInt32(row_vec[1]));
      table_.Insert(row);
    }
  }

  StringPool pool_;
  RawTable raw_{&pool_};
  tables::FtraceEventTable table_{&pool_, &raw_};
};

struct HeapGraphObjectTableForBenchmark {
  explicit HeapGraphObjectTableForBenchmark(benchmark::State& state) {
    std::vector<std::string> table_rows_as_string =
        ReadCSV(state, kHeapGraphObjectTable);

    for (size_t i = 1; i < table_rows_as_string.size(); ++i) {
      std::vector<std::string> row_vec = SplitCSVLine(table_rows_as_string[i]);

      HeapGraphObjectTable::Row row;
      row.upid = *base::StringToUInt32(row_vec[1]);
      row.reference_set_id = base::StringToUInt32(row_vec[2]);
      table_.Insert(row);
    }
  }
  StringPool pool_;
  HeapGraphObjectTable table_{&pool_};
};

void BenchmarkSliceTableFilter(benchmark::State& state,
                               SliceTableForBenchmark& table,
                               std::initializer_list<Constraint> c) {
  Query q;
  q.constraints = c;
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToIterator(q));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
  state.counters["s/out"] =
      benchmark::Counter(CountRows(table.table_.FilterToIterator(q)),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

void BenchmarkSliceTableSort(benchmark::State& state,
                             SliceTableForBenchmark& table,
                             std::initializer_list<Order> ob) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.Sort(ob));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

void BenchmarkExpectedFrameTableQuery(
    benchmark::State& state,
    ExpectedFrameTimelineTableForBenchmark& table,
    Query q) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToIterator(q));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
  state.counters["s/out"] =
      benchmark::Counter(CountRows(table.table_.FilterToIterator(q)),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

void BenchmarkFtraceEventTableQuery(benchmark::State& state,
                                    FtraceEventTableForBenchmark& table,
                                    Query q) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToIterator(q));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
  state.counters["s/out"] =
      benchmark::Counter(CountRows(table.table_.FilterToIterator(q)),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

void BenchmarkFtraceEventTableSort(benchmark::State& state,
                                   FtraceEventTableForBenchmark& table,
                                   std::initializer_list<Order> ob) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.Sort(ob));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}

void BM_QESliceTableTrackIdEq(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table, {table.table_.track_id().eq(1213)});
}
BENCHMARK(BM_QESliceTableTrackIdEq);

void BM_QESliceTableParentIdIsNotNull(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table,
                            {table.table_.parent_id().is_not_null()});
}
BENCHMARK(BM_QESliceTableParentIdIsNotNull);

void BM_QESliceTableParentIdEq(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table, {table.table_.parent_id().eq(26711)});
}
BENCHMARK(BM_QESliceTableParentIdEq);

void BM_QESliceTableNameEq(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(
      state, table,
      {table.table_.name().eq("MarkFromReadBarrierWithMeasurements")});
}
BENCHMARK(BM_QESliceTableNameEq);

void BM_QESliceTableNameGlobNoStars(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(
      state, table,
      {table.table_.name().glob("MarkFromReadBarrierWithMeasurements")});
}
BENCHMARK(BM_QESliceTableNameGlobNoStars);

void BM_QESliceTableNameGlob(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(
      state, table, {table.table_.name().glob("HIDL::IMapper::unlock::*")});
}
BENCHMARK(BM_QESliceTableNameGlob);

void BM_QESliceTableNameRegex(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table,
                            {table.table_.name().regex(".*Pool.*")});
}
BENCHMARK(BM_QESliceTableNameRegex);

void BM_QESliceTableSorted(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table,
                            {table.table_.ts().gt(1738923505854),
                             table.table_.ts().lt(1738950140556)});
}
BENCHMARK(BM_QESliceTableSorted);

void BM_QEFilterWithSparseSelector(benchmark::State& state) {
  ExpectedFrameTimelineTableForBenchmark table(state);
  Query q;
  q.constraints = {table.table_.track_id().eq(1445)};
  BenchmarkExpectedFrameTableQuery(state, table, q);
}
BENCHMARK(BM_QEFilterWithSparseSelector);

void BM_QEFilterWithDenseSelector(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  Query q;
  q.constraints = {table.table_.ucpu().eq(4)};
  BenchmarkFtraceEventTableQuery(state, table, q);
}
BENCHMARK(BM_QEFilterWithDenseSelector);

void BM_QESliceEventFilterId(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table, {table.table_.id().eq(500)});
}
BENCHMARK(BM_QESliceEventFilterId);

void BM_QEFtraceEventFilterId(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  Query q;
  q.constraints = {table.table_.id().eq(500)};
  BenchmarkFtraceEventTableQuery(state, table, q);
}

BENCHMARK(BM_QEFtraceEventFilterId);

void BM_QESliceTableTsAndTrackId(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(
      state, table,
      {table.table_.ts().ge(1738923505854), table.table_.ts().le(1738950140556),
       table.table_.track_id().eq(1422)});
}
BENCHMARK(BM_QESliceTableTsAndTrackId);

void BM_QEFilterOneElement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(
      state, table,
      {table.table_.id().eq(11732), table.table_.track_id().eq(1422)});
}
BENCHMARK(BM_QEFilterOneElement);

void BM_QEFilterWithArrangement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Order order{table.table_.dur().index_in_table(), false};
  Table slice_sorted_with_duration = table.table_.Sort({order});

  Constraint c{table.table_.track_id().index_in_table(), FilterOp::kGt,
               SqlValue::Long(10)};
  Query q;
  q.constraints = {c};
  for (auto _ : state) {
    benchmark::DoNotOptimize(slice_sorted_with_duration.QueryToRowMap(q));
  }
  state.counters["s/row"] = benchmark::Counter(
      static_cast<double>(slice_sorted_with_duration.row_count()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
  state.counters["s/out"] = benchmark::Counter(
      static_cast<double>(table.table_.QueryToRowMap(q).size()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEFilterWithArrangement);

void BM_QEDenseNullFilter(benchmark::State& state) {
  HeapGraphObjectTableForBenchmark table(state);
  Constraint c{table.table_.reference_set_id().index_in_table(), FilterOp::kGt,
               SqlValue::Long(1000)};
  Query q;
  q.constraints = {c};
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToIterator(q));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
  state.counters["s/out"] =
      benchmark::Counter(CountRows(table.table_.FilterToIterator(q)),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEDenseNullFilter);

void BM_QEDenseNullFilterIsNull(benchmark::State& state) {
  HeapGraphObjectTableForBenchmark table(state);
  Constraint c{table.table_.reference_set_id().index_in_table(),
               FilterOp::kIsNull, SqlValue()};
  Query q;
  q.constraints = {c};
  for (auto _ : state) {
    benchmark::DoNotOptimize(table.table_.FilterToIterator(q));
  }
  state.counters["s/row"] =
      benchmark::Counter(static_cast<double>(table.table_.row_count()),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
  state.counters["s/out"] =
      benchmark::Counter(CountRows(table.table_.FilterToIterator(q)),
                         benchmark::Counter::kIsIterationInvariantRate |
                             benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEDenseNullFilterIsNull);

void BM_QEIdColumnWithIntAsDouble(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Constraint c{table.table_.track_id().index_in_table(), FilterOp::kEq,
               SqlValue::Double(100)};
  BenchmarkSliceTableFilter(state, table, {c});
}
BENCHMARK(BM_QEIdColumnWithIntAsDouble);

void BM_QEIdColumnWithDouble(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Constraint c{table.table_.track_id().index_in_table(), FilterOp::kEq,
               SqlValue::Double(100.5)};
  BenchmarkSliceTableFilter(state, table, {c});
}
BENCHMARK(BM_QEIdColumnWithDouble);

void BM_QEFilterOrderedArrangement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Order order{table.table_.dur().index_in_table(), false};
  Table slice_sorted_with_duration = table.table_.Sort({order});

  Constraint c{table.table_.dur().index_in_table(), FilterOp::kGt,
               SqlValue::Long(10)};
  Query q;
  q.constraints = {c};
  for (auto _ : state) {
    benchmark::DoNotOptimize(slice_sorted_with_duration.QueryToRowMap(q));
  }
  state.counters["s/row"] = benchmark::Counter(
      static_cast<double>(slice_sorted_with_duration.row_count()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
  state.counters["s/out"] = benchmark::Counter(
      static_cast<double>(table.table_.QueryToRowMap(q).size()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEFilterOrderedArrangement);

void BM_QEFilterNullOrderedArrangement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Order order{table.table_.parent_id().index_in_table(), false};
  Table slice_sorted_with_parent_id = table.table_.Sort({order});

  Constraint c{table.table_.parent_id().index_in_table(), FilterOp::kGt,
               SqlValue::Long(26091)};
  Query q;
  q.constraints = {c};
  for (auto _ : state) {
    benchmark::DoNotOptimize(slice_sorted_with_parent_id.QueryToRowMap(q));
  }
  state.counters["s/row"] = benchmark::Counter(
      static_cast<double>(slice_sorted_with_parent_id.row_count()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
  state.counters["s/out"] = benchmark::Counter(
      static_cast<double>(table.table_.QueryToRowMap(q).size()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEFilterNullOrderedArrangement);

void BM_QESliceFilterIndexSearchOneElement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(
      state, table,
      {table.table_.track_id().eq(1422), table.table_.id().eq(11732)});
}
BENCHMARK(BM_QESliceFilterIndexSearchOneElement);

void BM_QESliceFilterIndexSearch(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableFilter(state, table,
                            {table.table_.track_id().eq(1422),
                             table.table_.name().eq("notifyFramePending")});
}
BENCHMARK(BM_QESliceFilterIndexSearch);

void BM_QESliceSortNumericAsc(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableSort(state, table, {table.table_.track_id().ascending()});
}
BENCHMARK(BM_QESliceSortNumericAsc);

void BM_QESliceSortNullNumericAsc(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  BenchmarkSliceTableSort(state, table, {table.table_.parent_id().ascending()});
}
BENCHMARK(BM_QESliceSortNullNumericAsc);

void BM_QEFtraceEventSortSelectorNumericAsc(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  BenchmarkFtraceEventTableSort(state, table,
                                {table.table_.ucpu().ascending()});
}
BENCHMARK(BM_QEFtraceEventSortSelectorNumericAsc);

void BM_QEFtraceEventSortSelectorNumericDesc(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  BenchmarkFtraceEventTableSort(state, table,
                                {table.table_.ucpu().descending()});
}
BENCHMARK(BM_QEFtraceEventSortSelectorNumericDesc);

void BM_QEDistinctWithSparseSelector(benchmark::State& state) {
  ExpectedFrameTimelineTableForBenchmark table(state);
  Query q;
  q.order_type = Query::OrderType::kDistinct;
  q.orders = {table.table_.track_id().descending()};
  BenchmarkExpectedFrameTableQuery(state, table, q);
}
BENCHMARK(BM_QEDistinctWithSparseSelector);

void BM_QEDistinctWithDenseSelector(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  Query q;
  q.order_type = Query::OrderType::kDistinct;
  q.orders = {table.table_.ucpu().descending()};
  BenchmarkFtraceEventTableQuery(state, table, q);
}
BENCHMARK(BM_QEDistinctWithDenseSelector);

void BM_QEDistinctSortedWithSparseSelector(benchmark::State& state) {
  ExpectedFrameTimelineTableForBenchmark table(state);
  Query q;
  q.order_type = Query::OrderType::kDistinctAndSort;
  q.orders = {table.table_.track_id().descending()};
  BenchmarkExpectedFrameTableQuery(state, table, q);
}
BENCHMARK(BM_QEDistinctSortedWithSparseSelector);

void BM_QEDistinctSortedWithDenseSelector(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  Query q;
  q.order_type = Query::OrderType::kDistinctAndSort;
  q.orders = {table.table_.ucpu().descending()};
  BenchmarkFtraceEventTableQuery(state, table, q);
}
BENCHMARK(BM_QEDistinctSortedWithDenseSelector);

void BM_QEDistinctWithArrangement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Order order{table.table_.dur().index_in_table(), false};
  Table slice_sorted_with_duration = table.table_.Sort({order});

  Query q;
  q.order_type = Query::OrderType::kDistinct;
  q.orders = {table.table_.track_id().descending()};

  for (auto _ : state) {
    benchmark::DoNotOptimize(slice_sorted_with_duration.QueryToRowMap(q));
  }
  state.counters["s/row"] = benchmark::Counter(
      static_cast<double>(slice_sorted_with_duration.row_count()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
  state.counters["s/out"] = benchmark::Counter(
      static_cast<double>(table.table_.QueryToRowMap(q).size()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEDistinctWithArrangement);

void BM_QEDistinctSortedWithArrangement(benchmark::State& state) {
  SliceTableForBenchmark table(state);
  Order order{table.table_.dur().index_in_table(), false};
  Table slice_sorted_with_duration = table.table_.Sort({order});

  Query q;
  q.order_type = Query::OrderType::kDistinctAndSort;
  q.orders = {table.table_.track_id().descending()};

  for (auto _ : state) {
    benchmark::DoNotOptimize(slice_sorted_with_duration.QueryToRowMap(q));
  }
  state.counters["s/row"] = benchmark::Counter(
      static_cast<double>(slice_sorted_with_duration.row_count()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
  state.counters["s/out"] = benchmark::Counter(
      static_cast<double>(table.table_.QueryToRowMap(q).size()),
      benchmark::Counter::kIsIterationInvariantRate |
          benchmark::Counter::kInvert);
}
BENCHMARK(BM_QEDistinctSortedWithArrangement);

void BM_QEOffsetLimit(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  Query q;
  q.limit = 10;
  q.offset = 100;
  BenchmarkFtraceEventTableQuery(state, table, q);
}
BENCHMARK(BM_QEOffsetLimit);

void BM_QEMax(benchmark::State& state) {
  FtraceEventTableForBenchmark table(state);
  Query q;
  q.limit = 1;
  q.orders = {table.table_.utid().descending()};
  BenchmarkFtraceEventTableQuery(state, table, q);
}
BENCHMARK(BM_QEMax);

}  // namespace
}  // namespace perfetto::trace_processor
