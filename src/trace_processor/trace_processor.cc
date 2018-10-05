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

#include "src/trace_processor/trace_processor.h"

#include <sqlite3.h>
#include <functional>

#include "perfetto/base/time.h"
#include "src/trace_processor/counters_table.h"
#include "src/trace_processor/json_trace_parser.h"
#include "src/trace_processor/process_table.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/proto_trace_tokenizer.h"
#include "src/trace_processor/sched_slice_table.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/slice_table.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/span_operator_table.h"
#include "src/trace_processor/string_table.h"
#include "src/trace_processor/table.h"
#include "src/trace_processor/thread_table.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/window_operator_table.h"

#include "perfetto/trace_processor/raw_query.pb.h"

namespace perfetto {
namespace trace_processor {

TraceProcessor::TraceProcessor(const Config& cfg) {
  sqlite3* db = nullptr;
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  db_.reset(std::move(db));

  context_.storage.reset(new TraceStorage());
  context_.slice_tracker.reset(new SliceTracker(&context_));
  context_.sched_tracker.reset(new SchedTracker(&context_));
  context_.proto_parser.reset(new ProtoTraceParser(&context_));
  context_.process_tracker.reset(new ProcessTracker(&context_));
  context_.sorter.reset(
      new TraceSorter(&context_, cfg.optimization_mode, cfg.window_size_ns));

  ProcessTable::RegisterTable(*db_, context_.storage.get());
  SchedSliceTable::RegisterTable(*db_, context_.storage.get());
  SliceTable::RegisterTable(*db_, context_.storage.get());
  StringTable::RegisterTable(*db_, context_.storage.get());
  ThreadTable::RegisterTable(*db_, context_.storage.get());
  CountersTable::RegisterTable(*db_, context_.storage.get());
  SpanOperatorTable::RegisterTable(*db_, context_.storage.get());
  WindowOperatorTable::RegisterTable(*db_, context_.storage.get());
}

TraceProcessor::~TraceProcessor() = default;

bool TraceProcessor::Parse(std::unique_ptr<uint8_t[]> data, size_t size) {
  if (size == 0)
    return true;
  if (unrecoverable_parse_error_)
    return false;

  // If this is the first Parse() call, guess the trace type and create the
  // appropriate parser.
  if (!context_.chunk_reader) {
    char buf[32];
    memcpy(buf, &data[0], std::min(size, sizeof(buf)));
    buf[sizeof(buf) - 1] = '\0';
    const size_t kPreambleLen = strlen(JsonTraceParser::kPreamble);
    if (strncmp(buf, JsonTraceParser::kPreamble, kPreambleLen) == 0) {
      PERFETTO_DLOG("Legacy JSON trace detected");
      context_.chunk_reader.reset(new JsonTraceParser(&context_));
    } else {
      context_.chunk_reader.reset(new ProtoTraceTokenizer(&context_));
    }
  }

  bool res = context_.chunk_reader->Parse(std::move(data), size);
  unrecoverable_parse_error_ |= !res;
  return res;
}

void TraceProcessor::NotifyEndOfFile() {
  context_.sorter->FlushEventsForced();
}

void TraceProcessor::ExecuteQuery(
    const protos::RawQueryArgs& args,
    std::function<void(const protos::RawQueryResult&)> callback) {
  protos::RawQueryResult proto;
  query_interrupted_.store(false, std::memory_order_relaxed);

  base::TimeNanos t_start = base::GetWallTimeNs();

  const auto& sql = args.sql_query();
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

    for (int i = 0; i < col_count; i++) {
      if (row_count == 0) {
        // Setup the descriptors.
        auto* descriptor = proto.add_column_descriptors();
        descriptor->set_name(sqlite3_column_name(*stmt, i));

        switch (sqlite3_column_type(*stmt, i)) {
          case SQLITE_INTEGER:
            descriptor->set_type(protos::RawQueryResult_ColumnDesc_Type_LONG);
            break;
          case SQLITE_TEXT:
            descriptor->set_type(protos::RawQueryResult_ColumnDesc_Type_STRING);
            break;
          case SQLITE_FLOAT:
            descriptor->set_type(protos::RawQueryResult_ColumnDesc_Type_DOUBLE);
            break;
          case SQLITE_NULL:
            proto.set_error("Query yields to NULL column, can't handle that");
            callback(std::move(proto));
            return;
        }

        // Add an empty column.
        proto.add_columns();
      }

      auto* column = proto.mutable_columns(i);
      switch (proto.column_descriptors(i).type()) {
        case protos::RawQueryResult_ColumnDesc_Type_LONG:
          column->add_long_values(sqlite3_column_int64(*stmt, i));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_STRING: {
          const char* str =
              reinterpret_cast<const char*>(sqlite3_column_text(*stmt, i));
          column->add_string_values(str ? str : "[NULL]");
          break;
        }
        case protos::RawQueryResult_ColumnDesc_Type_DOUBLE:
          column->add_double_values(sqlite3_column_double(*stmt, i));
          break;
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
  proto.set_execution_time_ns(static_cast<uint64_t>((t_end - t_start).count()));
  callback(proto);
}

void TraceProcessor::InterruptQuery() {
  if (!db_)
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(db_.get());
}

// static
void EnableSQLiteVtableDebugging() {
  // This level of indirection is required to avoid clients to depend on table.h
  // which in turn requires sqlite headers.
  Table::debug = true;
}

}  // namespace trace_processor
}  // namespace perfetto
