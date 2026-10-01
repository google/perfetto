/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/storage/trace_storage.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/tables/all_tables_fwd.h"
#include "src/trace_processor/tables/android_tables_py.h"   // IWYU pragma: keep
#include "src/trace_processor/tables/counter_tables_py.h"   // IWYU pragma: keep
#include "src/trace_processor/tables/etm_tables_py.h"       // IWYU pragma: keep
#include "src/trace_processor/tables/flow_tables_py.h"      // IWYU pragma: keep
#include "src/trace_processor/tables/jit_tables_py.h"       // IWYU pragma: keep
#include "src/trace_processor/tables/log_tables_py.h"       // IWYU pragma: keep
#include "src/trace_processor/tables/memory_tables_py.h"    // IWYU pragma: keep
#include "src/trace_processor/tables/metadata_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/tables/perf_tables_py.h"      // IWYU pragma: keep
#include "src/trace_processor/tables/profiler_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/tables/sched_tables_py.h"     // IWYU pragma: keep
#include "src/trace_processor/tables/slice_tables_py.h"     // IWYU pragma: keep
#include "src/trace_processor/tables/state_tables_py.h"     // IWYU pragma: keep
#include "src/trace_processor/tables/trace_proto_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/tables/track_tables_py.h"     // IWYU pragma: keep
#include "src/trace_processor/tables/v8_tables_py.h"        // IWYU pragma: keep
#include "src/trace_processor/tables/winscope_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

namespace {

// Struct holding the parameters needed to initialize a table.
struct TableInitParams {
  uint32_t column_count;
  const char* const* column_names;
  const dataframe::ColumnSpec* column_specs;
  const bool* retained_columns;
  const char* name;
};

template <class Variant, class T>
struct table_init_params;

template <class T, class... Ts>
struct table_init_params<std::variant<Ts...>, T> {
  static constexpr std::array<TableInitParams, sizeof...(Ts)> value = {
      {{decltype(Ts::kSpec)::kColumnCount, Ts::kSpec.column_names.data(),
        Ts::kSpec.column_specs.data(), Ts::kRetainedColumns, Ts::Name()}...}};
};

// Array of initialization parameters for all tables, in the same order as
// AllTables variant. This allows us to initialize all tables in a simple loop
// without template instantiation bloat.
constexpr std::array kTableInitParams =
    table_init_params<tables::AllTables, void>::value;

static_assert(
    std::size(kTableInitParams) == tables::kTableCount,
    "kTableInitParams must have the same number of entries as tables");

}  // namespace

dataframe::Dataframe* TraceStorage::FindDataframeForOutput(const char* name) {
  for (size_t i = 0; i < tables::kTableCount; ++i) {
    if (strcmp(kTableInitParams[i].name, name) == 0)
      return reinterpret_cast<dataframe::Dataframe*>(
          &tables_storage_[i * sizeof(dataframe::Dataframe)]);
  }
  return nullptr;
}

void TraceStorage::EnableFtraceStreaming() {
  if (ftrace_sched_sink_only_)
    return;
  ftrace_sched_sink_only_ = true;
  for (const char* name :
       {tables::SchedSliceTable::Name(), tables::ThreadStateTable::Name()}) {
    auto* df = FindDataframeForOutput(name);
    PERFETTO_CHECK(df && df->row_count() == 0);
    df->ConfigureStreaming(
        table_sink_factory_ ? table_sink_factory_(name) : nullptr,
        TableRetentionMask(df, name));
  }
}

void TraceStorage::AdvanceFtraceSchedFrontier(uint32_t ucpu,
                                              uint32_t row,
                                              uint32_t batch_rows) {
  PERFETTO_CHECK(batch_rows > 0);
  auto* table = mutable_sched_slice_table();
  PERFETTO_CHECK(table->dataframe().streaming_configured());
  auto [it, inserted] = ftrace_sched_frontiers_.emplace(ucpu, row);
  PERFETTO_CHECK(inserted || row >= it->second);
  it->second = row;
  if (table->row_count() - sched_frontier_last_check_ < batch_rows)
    return;
  sched_frontier_last_check_ = table->row_count();
  uint32_t frontier = table->row_count();
  for (const auto& pin : ftrace_sched_frontiers_)
    frontier = std::min(frontier, pin.second);
  // CPU state supplies the start timestamp; no sched values need local reads.
  // Completion still waits for every CPU, while storage can pass open rows.
  table->DropRowsBefore(table->row_count());
  table->dataframe().AdvanceCompletionFrontier(frontier);
}

void TraceStorage::AdvanceFtraceThreadStateStorageFrontier(
    uint32_t batch_rows) {
  PERFETTO_CHECK(batch_rows > 0);
  auto* table = mutable_thread_state_table();
  if (table->row_count() - table->dataframe().first_retained_row() >=
      batch_rows)
    table->DropRowsBefore(table->row_count());
  // Sleeping threads and late blocked-reason patches keep completion separate.
  // This prototype makes no premature completion promise for thread states.
}

void TraceStorage::AdvanceSliceStorageFrontier(uint32_t batch_rows) {
  PERFETTO_CHECK(batch_rows > 0);
  auto* table = mutable_slice_table();
  if (table->row_count() - table->dataframe().first_retained_row() >=
      batch_rows)
    table->DropRowsBefore(table->row_count());
}

void TraceStorage::AdvanceArgsStorageFrontier() {
  if (!slice_args_sink_only_)
    return;
  PERFETTO_CHECK(streaming_batch_rows_ > 0);
  auto* table = mutable_arg_table();
  auto& df = table->dataframe();
  if (table->row_count() - df.first_retained_row() >= streaming_batch_rows_) {
    table->DropRowsBefore(table->row_count());
    // Arg rows are immutable after each arg set is inserted.
    df.AdvanceCompletionFrontier(table->row_count());
  }
}

void TraceStorage::AdvanceCounterStorageFrontier() {
  if (!counter_sink_only_)
    return;
  auto* table = mutable_counter_table();
  if (table->row_count() - table->dataframe().first_retained_row() >=
      streaming_batch_rows_)
    table->DropRowsBefore(table->row_count());
  // Deferred track resolution and backwards-looking values can patch old rows.
}

TraceStorage::TraceStorage()
    : TraceStorage([] {
        TraceParserOptions config;
        config.drop_unread_table_columns = false;
        return config;
      }()) {}

TraceStorage::TraceStorage(const TraceParserOptions& config)
    : drop_unread_columns_(config.drop_unread_table_columns),
      ftrace_sched_sink_only_(config.experimental_ftrace_sched_frontier),
      slice_args_sink_only_(config.experimental_slice_args_streaming),
      counter_sink_only_(config.counter_sink_only),
      streaming_batch_rows_(config.streaming_frontier_batch_rows) {
  // Initialize all tables using placement new in a simple loop.
  for (size_t i = 0; i < tables::kTableCount; ++i) {
    const auto& params = kTableInitParams[i];
    auto* df = new (&tables_storage_[i * sizeof(dataframe::Dataframe)])
        dataframe::Dataframe(&string_pool_, params.column_count,
                             params.column_names, params.column_specs);
    df->SetParserRetentionMask(params.retained_columns);
    ConfigureTableOutput(df, params.name);
  }
  for (uint32_t i = 0; i < variadic_type_ids_.size(); ++i) {
    variadic_type_ids_[i] = InternString(Variadic::kTypeNames[i]);
  }
  if (config.on_trace_storage_created) {
    config.on_trace_storage_created(this);
  }
}

std::vector<bool> TraceStorage::TableRetentionMask(dataframe::Dataframe* df,
                                                   const char* name) const {
  if (!drop_unread_columns_)
    return {};
  auto mask = df->ParserRetentionMask();
  if (strcmp(name, tables::FtraceEventTable::Name()) == 0) {
    // All C++ reads are annotated POST_FINALIZATION. TraceParser has no query
    // or export finalization, so these records can be emitted without storage.
    std::fill(mask.begin() + 1, mask.end(), false);
  }
  if (ftrace_sched_sink_only_ &&
      (strcmp(name, tables::SchedSliceTable::Name()) == 0 ||
       strcmp(name, tables::ThreadStateTable::Name()) == 0)) {
    std::fill(mask.begin() + 1, mask.end(), false);
  }
  if (counter_sink_only_ && strcmp(name, tables::CounterTable::Name()) == 0)
    std::fill(mask.begin() + 1, mask.end(), false);
  if (slice_args_sink_only_ && (strcmp(name, tables::SliceTable::Name()) == 0 ||
                                strcmp(name, tables::ArgTable::Name()) == 0)) {
    std::fill(mask.begin() + 1, mask.end(), false);
  }
  return mask;
}

void TraceStorage::SetTableSinks(const TableSinkFactory& factory,
                                 bool drop_unread_columns) {
  table_sink_factory_ = factory;
  drop_unread_columns_ = drop_unread_columns;
  for (size_t i = 0; i < tables::kTableCount; ++i) {
    const auto& params = kTableInitParams[i];
    auto* df = reinterpret_cast<dataframe::Dataframe*>(
        &tables_storage_[i * sizeof(dataframe::Dataframe)]);
    df->ConfigureStreaming(
        table_sink_factory_ ? table_sink_factory_(params.name) : nullptr,
        TableRetentionMask(df, params.name));
  }
}

void TraceStorage::ConfigureTableOutput(dataframe::Dataframe* df,
                                        const char* name) {
  if (df->streaming_configured() ||
      (!table_sink_factory_ && !drop_unread_columns_)) {
    return;
  }
  df->ConfigureStreaming(
      table_sink_factory_ ? table_sink_factory_(name) : nullptr,
      TableRetentionMask(df, name));
}

TraceStorage::~TraceStorage() {
  // Destroy all tables in reverse order of construction.
  for (size_t i = tables::kTableCount; i > 0; --i) {
    reinterpret_cast<dataframe::Dataframe*>(
        &tables_storage_[(i - 1) * sizeof(dataframe::Dataframe)])
        ->~Dataframe();
  }
}

uint32_t TraceStorage::SqlStats::RecordQueryBegin(const std::string& query,
                                                  int64_t time_started) {
  if (queries_.size() >= kMaxLogEntries) {
    queries_.pop_front();
    times_started_.pop_front();
    times_first_next_.pop_front();
    times_ended_.pop_front();
    popped_queries_++;
  }
  queries_.push_back(query);
  times_started_.push_back(time_started);
  times_first_next_.push_back(0);
  times_ended_.push_back(0);
  return static_cast<uint32_t>(popped_queries_ + queries_.size() - 1);
}

void TraceStorage::SqlStats::RecordQueryFirstNext(uint32_t row,
                                                  int64_t time_first_next) {
  // This means we've popped this query off the queue of queries before it had
  // a chance to finish. Just silently drop this number.
  if (popped_queries_ > row)
    return;
  uint32_t queue_row = row - popped_queries_;
  PERFETTO_DCHECK(queue_row < queries_.size());
  times_first_next_[queue_row] = time_first_next;
}

void TraceStorage::SqlStats::RecordQueryEnd(uint32_t row, int64_t time_ended) {
  // This means we've popped this query off the queue of queries before it had
  // a chance to finish. Just silently drop this number.
  if (popped_queries_ > row)
    return;
  uint32_t queue_row = row - popped_queries_;
  PERFETTO_DCHECK(queue_row < queries_.size());
  times_ended_[queue_row] = time_ended;
}

}  // namespace perfetto::trace_processor
