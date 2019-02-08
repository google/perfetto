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

#include "src/trace_processor/trace_processor_impl.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <algorithm>
#include <functional>

#include "perfetto/base/time.h"
#include "src/trace_processor/android_logs_table.h"
#include "src/trace_processor/args_table.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/counters_table.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/instants_table.h"
#include "src/trace_processor/process_table.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/proto_trace_tokenizer.h"
#include "src/trace_processor/raw_table.h"
#include "src/trace_processor/sched_slice_table.h"
#include "src/trace_processor/slice_table.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/span_join_operator_table.h"
#include "src/trace_processor/sql_stats_table.h"
#include "src/trace_processor/stats_table.h"
#include "src/trace_processor/string_table.h"
#include "src/trace_processor/table.h"
#include "src/trace_processor/thread_table.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/window_operator_table.h"

#include "perfetto/trace_processor/raw_query.pb.h"

// JSON parsing is only supported in the standalone build.
#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#include "src/trace_processor/json_trace_parser.h"
#endif

// In Android tree builds, we don't have the percentile module.
// Just don't include it.
#if !PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
// defined in sqlite_src/ext/misc/percentile.c
extern "C" int sqlite3_percentile_init(sqlite3* db,
                                       char** error,
                                       const sqlite3_api_routines* api);
#endif

namespace {
void InitializeSqliteModules(sqlite3* db) {
// In Android tree builds, we don't have the percentile module.
// Just don't include it.
#if !PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  char* error = nullptr;
  sqlite3_percentile_init(db, &error, nullptr);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
#endif
}

void CreateBuiltinTables(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "CREATE TABLE perfetto_tables(name STRING)", 0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  sqlite3_exec(db,
               "CREATE TABLE trace_bounds(start_ts BIG INT, end_ts BIG INT)", 0,
               0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
}

void BuildBoundsTable(sqlite3* db, std::pair<int64_t, int64_t> bounds) {
  char* insert_sql = sqlite3_mprintf("INSERT INTO trace_bounds VALUES(%" PRId64
                                     ", %" PRId64 ")",
                                     bounds.first, bounds.second);
  char* error = nullptr;
  sqlite3_exec(db, insert_sql, 0, 0, &error);
  sqlite3_free(insert_sql);
  if (error) {
    PERFETTO_ELOG("Error inserting bounds table: %s", error);
    sqlite3_free(error);
  }
}
}  // namespace

namespace perfetto {
namespace trace_processor {
namespace {

bool IsPrefix(const std::string& a, const std::string& b) {
  return a.size() <= b.size() && b.substr(0, a.size()) == a;
}

std::string RemoveWhitespace(const std::string& input) {
  std::string str(input);
  str.erase(std::remove_if(str.begin(), str.end(), ::isspace), str.end());
  return str;
}

}  // namespace

TraceType GuessTraceType(const uint8_t* data, size_t size) {
  if (size == 0)
    return kUnknownTraceType;
  std::string start(reinterpret_cast<const char*>(data),
                    std::min<size_t>(size, 20));
  std::string start_minus_white_space = RemoveWhitespace(start);
  if (IsPrefix("{\"traceEvents\":[", start_minus_white_space))
    return kJsonTraceType;
  if (IsPrefix("[{", start_minus_white_space))
    return kJsonTraceType;
  return kProtoTraceType;
}

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg) {
  sqlite3* db = nullptr;
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqliteModules(db);
  CreateBuiltinTables(db);
  db_.reset(std::move(db));

  context_.storage.reset(new TraceStorage());
  context_.args_tracker.reset(new ArgsTracker(&context_));
  context_.slice_tracker.reset(new SliceTracker(&context_));
  context_.event_tracker.reset(new EventTracker(&context_));
  context_.proto_parser.reset(new ProtoTraceParser(&context_));
  context_.process_tracker.reset(new ProcessTracker(&context_));
  context_.clock_tracker.reset(new ClockTracker(&context_));
  context_.sorter.reset(
      new TraceSorter(&context_, cfg.optimization_mode,
                      static_cast<int64_t>(cfg.window_size_ns)));

  ArgsTable::RegisterTable(*db_, context_.storage.get());
  ProcessTable::RegisterTable(*db_, context_.storage.get());
  SchedSliceTable::RegisterTable(*db_, context_.storage.get());
  SliceTable::RegisterTable(*db_, context_.storage.get());
  SqlStatsTable::RegisterTable(*db_, context_.storage.get());
  StringTable::RegisterTable(*db_, context_.storage.get());
  ThreadTable::RegisterTable(*db_, context_.storage.get());
  CountersTable::RegisterTable(*db_, context_.storage.get());
  SpanJoinOperatorTable::RegisterTable(*db_, context_.storage.get());
  WindowOperatorTable::RegisterTable(*db_, context_.storage.get());
  InstantsTable::RegisterTable(*db_, context_.storage.get());
  StatsTable::RegisterTable(*db_, context_.storage.get());
  AndroidLogsTable::RegisterTable(*db_, context_.storage.get());
  RawTable::RegisterTable(*db_, context_.storage.get());
}

TraceProcessorImpl::~TraceProcessorImpl() = default;

bool TraceProcessorImpl::Parse(std::unique_ptr<uint8_t[]> data, size_t size) {
  if (size == 0)
    return true;
  if (unrecoverable_parse_error_)
    return false;

  // If this is the first Parse() call, guess the trace type and create the
  // appropriate parser.
  if (!context_.chunk_reader) {
    TraceType trace_type = GuessTraceType(data.get(), size);
    switch (trace_type) {
      case kJsonTraceType:
        PERFETTO_DLOG("Legacy JSON trace detected");
#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
        context_.chunk_reader.reset(new JsonTraceParser(&context_));
#else
        PERFETTO_FATAL("JSON traces only supported in standalone mode.");
#endif
        break;
      case kProtoTraceType:
        context_.chunk_reader.reset(new ProtoTraceTokenizer(&context_));
        break;
      case kUnknownTraceType:
        return false;
    }
  }

  bool res = context_.chunk_reader->Parse(std::move(data), size);
  unrecoverable_parse_error_ |= !res;
  return res;
}

void TraceProcessorImpl::NotifyEndOfFile() {
  context_.sorter->FlushEventsForced();
  BuildBoundsTable(*db_, context_.storage->GetTraceTimestampBoundsNs());
}

void TraceProcessorImpl::ExecuteQuery(
    const protos::RawQueryArgs& args,
    std::function<void(const protos::RawQueryResult&)> callback) {
  protos::RawQueryResult proto;
  query_interrupted_.store(false, std::memory_order_relaxed);

  base::TimeNanos t_start = base::GetWallTimeNs();
  const std::string& sql = args.sql_query();
  context_.storage->mutable_sql_stats()->RecordQueryBegin(
      sql, static_cast<int64_t>(args.time_queued_ns()), t_start.count());
  sqlite3_stmt* raw_stmt;
  int err = sqlite3_prepare_v2(*db_, sql.c_str(), static_cast<int>(sql.size()),
                               &raw_stmt, nullptr);
  ScopedStmt stmt(raw_stmt);

  int col_count = sqlite3_column_count(*stmt);
  int row_count = 0;

  while (!err) {
    int r = sqlite3_step(*stmt);
    if (r != SQLITE_ROW) {
      if (r != SQLITE_DONE)
        err = r;
      break;
    }

    using ColumnDesc = protos::RawQueryResult::ColumnDesc;
    for (int col = 0; col < col_count; col++) {
      if (row_count == 0) {
        // Setup the descriptors.
        auto* descriptor = proto.add_column_descriptors();
        descriptor->set_name(sqlite3_column_name(*stmt, col));
        descriptor->set_type(ColumnDesc::UNKNOWN);

        // Add an empty column.
        proto.add_columns();
      }

      auto* column = proto.mutable_columns(col);
      auto* desc = proto.mutable_column_descriptors(col);
      auto col_type = sqlite3_column_type(*stmt, col);
      if (desc->type() == ColumnDesc::UNKNOWN) {
        switch (col_type) {
          case SQLITE_INTEGER:
            desc->set_type(ColumnDesc::LONG);
            break;
          case SQLITE_TEXT:
            desc->set_type(ColumnDesc::STRING);
            break;
          case SQLITE_FLOAT:
            desc->set_type(ColumnDesc::DOUBLE);
            break;
          case SQLITE_NULL:
            break;
        }
      }

      // If either the column type is null or we still don't know the type,
      // just add null values to all the columns.
      if (col_type == SQLITE_NULL || desc->type() == ColumnDesc::UNKNOWN) {
        column->add_long_values(0);
        column->add_string_values("[NULL]");
        column->add_double_values(0);
        column->add_is_nulls(true);
        continue;
      }

      // Cast the sqlite value to the type of the column.
      switch (desc->type()) {
        case ColumnDesc::LONG:
          column->add_long_values(sqlite3_column_int64(*stmt, col));
          column->add_is_nulls(false);
          break;
        case ColumnDesc::STRING: {
          const char* str =
              reinterpret_cast<const char*>(sqlite3_column_text(*stmt, col));
          column->add_string_values(str);
          column->add_is_nulls(false);
          break;
        }
        case ColumnDesc::DOUBLE:
          column->add_double_values(sqlite3_column_double(*stmt, col));
          column->add_is_nulls(false);
          break;
        case ColumnDesc::UNKNOWN:
          PERFETTO_FATAL("Handled in if statement above.");
      }
    }
    row_count++;
  }

  if (err) {
    proto.set_error(sqlite3_errmsg(*db_));
    callback(std::move(proto));
    return;
  }

  proto.set_num_records(static_cast<uint64_t>(row_count));

  if (query_interrupted_.load()) {
    PERFETTO_ELOG("SQLite query interrupted");
    query_interrupted_ = false;
  }

  base::TimeNanos t_end = base::GetWallTimeNs();
  context_.storage->mutable_sql_stats()->RecordQueryEnd(t_end.count());
  proto.set_execution_time_ns(static_cast<uint64_t>((t_end - t_start).count()));
  callback(proto);
}

void TraceProcessorImpl::InterruptQuery() {
  if (!db_)
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(db_.get());
}

}  // namespace trace_processor
}  // namespace perfetto
