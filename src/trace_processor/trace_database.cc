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
static const char kUIntColumnName[] = "UNSIGNED INT";
static const char kULongColumnName[] = "UNSIGNED BIG INT";

bool IsColumnUInt(const char* type) {
  return strncmp(type, kUIntColumnName, sizeof(kUIntColumnName)) == 0;
}

bool IsColumnULong(const char* type) {
  return strncmp(type, kULongColumnName, sizeof(kULongColumnName)) == 0;
}
}  // namespace

TraceDatabase::TraceDatabase(base::TaskRunner* task_runner)
    : task_runner_(task_runner), weak_factory_(this) {
  sqlite3_open(":memory:", &db_);

  // Setup the sched slice table.
  static sqlite3_module module = SchedSliceTable::CreateModule();
  sqlite3_create_module(db_, "sched", &module, static_cast<void*>(&storage_));
}

TraceDatabase::~TraceDatabase() {
  sqlite3_close(db_);
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
  sqlite3_stmt* stmt;
  int err = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()),
                               &stmt, nullptr);
  if (err) {
    callback(std::move(proto));
    return;
  }

  for (int i = 0, size = sqlite3_column_count(stmt); i < size; i++) {
    // Setup the descriptors.
    auto* descriptor = proto.add_column_descriptors();
    descriptor->set_name(sqlite3_column_name(stmt, i));

    const char* type = sqlite3_column_decltype(stmt, i);
    if (IsColumnUInt(type)) {
      descriptor->set_type(protos::RawQueryResult_ColumnDesc_Type_UNSIGNED_INT);
    } else if (IsColumnULong(type)) {
      descriptor->set_type(
          protos::RawQueryResult_ColumnDesc_Type_UNSIGNED_LONG);
    } else {
      PERFETTO_FATAL("Unexpected column type found in SQL query");
    }

    // Add the empty column to the proto.
    proto.add_columns();
  }

  int row_count = 0;
  for (int r = sqlite3_step(stmt); r == SQLITE_ROW; r = sqlite3_step(stmt)) {
    for (int i = 0; i < proto.columns_size(); i++) {
      auto* column = proto.mutable_columns(i);
      switch (proto.column_descriptors(i).type()) {
        case protos::RawQueryResult_ColumnDesc_Type_UNSIGNED_LONG:
          column->add_ulong_values(
              static_cast<uint64_t>(sqlite3_column_int64(stmt, i)));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_UNSIGNED_INT:
          column->add_uint_values(
              static_cast<uint32_t>(sqlite3_column_double(stmt, i)));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_INT:
        case protos::RawQueryResult_ColumnDesc_Type_LONG:
        case protos::RawQueryResult_ColumnDesc_Type_STRING:
          PERFETTO_FATAL("Unexpected column type found in SQL query");
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
