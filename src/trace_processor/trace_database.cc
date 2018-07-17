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

#include "src/trace_processor/trace_database.h"

#include <functional>

namespace perfetto {
namespace trace_processor {
namespace {
constexpr uint32_t kTraceChunkSizeB = 16 * 1024 * 1024;  // 16 MB
}  // namespace

TraceDatabase::TraceDatabase(base::TaskRunner* task_runner)
    : task_runner_(task_runner), weak_factory_(this) {
  sqlite3* db = nullptr;
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  db_.reset(std::move(db));

  // Setup the sched slice table.
  static sqlite3_module s_module = SchedSliceTable::CreateModule();
  sqlite3_create_module(*db_, "sched", &s_module,
                        static_cast<void*>(&storage_));

  // Setup the process table.
  static sqlite3_module p_module = ProcessTable::CreateModule();
  sqlite3_create_module(*db_, "process", &p_module,
                        static_cast<void*>(&storage_));

  // Setup the thread table.
  static sqlite3_module t_module = ThreadTable::CreateModule();
  sqlite3_create_module(*db_, "thread", &t_module,
                        static_cast<void*>(&storage_));
}

void TraceDatabase::LoadTrace(BlobReader* reader,
                              std::function<void()> callback) {
  // Reset storage and start a new trace parsing task.
  storage_ = {};
  parser_.reset(new TraceParser(reader, &storage_, kTraceChunkSizeB));
  LoadTraceChunk(callback);
}

void TraceDatabase::ExecuteQuery(
    const protos::RawQueryArgs& args,
    std::function<void(protos::RawQueryResult)> callback) {
  protos::RawQueryResult proto;

  const auto& sql = args.sql_query();
  sqlite3_stmt* raw_stmt;
  int err = sqlite3_prepare_v2(*db_, sql.c_str(), static_cast<int>(sql.size()),
                               &raw_stmt, nullptr);
  ScopedStmt stmt(std::move(raw_stmt));
  if (err) {
    callback(std::move(proto));
    return;
  }

  int col_count = sqlite3_column_count(*stmt);
  int row_count = 0;
  for (int r = sqlite3_step(*stmt); r == SQLITE_ROW; r = sqlite3_step(*stmt)) {
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
            PERFETTO_CHECK(false);
            break;
        }

        // Add an empty column.
        proto.add_columns();
      }

      auto* column = proto.mutable_columns(i);
      switch (proto.column_descriptors(i).type()) {
        case protos::RawQueryResult_ColumnDesc_Type_LONG:
          column->add_long_values(sqlite3_column_int64(*stmt, i));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_STRING:
          column->add_string_values(
              reinterpret_cast<const char*>(sqlite3_column_text(*stmt, i)));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_DOUBLE:
          column->add_double_values(sqlite3_column_double(*stmt, i));
          break;
      }
    }
    row_count++;
  }
  proto.set_num_records(static_cast<uint64_t>(row_count));

  callback(std::move(proto));
}

void TraceDatabase::LoadTraceChunk(std::function<void()> callback) {
  bool has_more = parser_->ParseNextChunk();
  if (!has_more) {
    callback();
    return;
  }

  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_this, callback] {
    if (!weak_this)
      return;

    weak_this->LoadTraceChunk(callback);
  });
}

}  // namespace trace_processor
}  // namespace perfetto
