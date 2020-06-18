/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_ITERATOR_IMPL_H_
#define SRC_TRACE_PROCESSOR_ITERATOR_IMPL_H_

#include <sqlite3.h>

#include <memory>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/export.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/sqlite/scoped_db.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorImpl;

class IteratorImpl {
 public:
  IteratorImpl(TraceProcessorImpl* impl,
               sqlite3* db,
               ScopedStmt,
               uint32_t column_count,
               util::Status,
               uint32_t sql_stats_row);
  ~IteratorImpl();

  IteratorImpl(IteratorImpl&) noexcept = delete;
  IteratorImpl& operator=(IteratorImpl&) = delete;

  IteratorImpl(IteratorImpl&&) noexcept = default;
  IteratorImpl& operator=(IteratorImpl&&) = default;

  // Methods called by the base Iterator class.
  bool Next() {
    // Delegate to the cc file to prevent trace_storage.h include in this file.
    if (!called_next_) {
      RecordFirstNextInSqlStats();
      called_next_ = true;
    }

    if (!status_.ok())
      return false;

    int ret = sqlite3_step(*stmt_);
    if (PERFETTO_UNLIKELY(ret != SQLITE_ROW && ret != SQLITE_DONE)) {
      status_ = util::ErrStatus("%s", sqlite3_errmsg(db_));
      return false;
    }
    return ret == SQLITE_ROW;
  }

  SqlValue Get(uint32_t col) {
    auto column = static_cast<int>(col);
    auto col_type = sqlite3_column_type(*stmt_, column);
    SqlValue value;
    switch (col_type) {
      case SQLITE_INTEGER:
        value.type = SqlValue::kLong;
        value.long_value = sqlite3_column_int64(*stmt_, column);
        break;
      case SQLITE_TEXT:
        value.type = SqlValue::kString;
        value.string_value =
            reinterpret_cast<const char*>(sqlite3_column_text(*stmt_, column));
        break;
      case SQLITE_FLOAT:
        value.type = SqlValue::kDouble;
        value.double_value = sqlite3_column_double(*stmt_, column);
        break;
      case SQLITE_BLOB:
        value.type = SqlValue::kBytes;
        value.bytes_value = sqlite3_column_blob(*stmt_, column);
        value.bytes_count =
            static_cast<size_t>(sqlite3_column_bytes(*stmt_, column));
        break;
      case SQLITE_NULL:
        value.type = SqlValue::kNull;
        break;
    }
    return value;
  }

  std::string GetColumnName(uint32_t col) {
    return sqlite3_column_name(stmt_.get(), static_cast<int>(col));
  }

  uint32_t ColumnCount() { return column_count_; }

  util::Status Status() { return status_; }

 private:
  // Dummy function to pass to ScopedResource.
  static int DummyClose(TraceProcessorImpl*) { return 0; }

  // Iterators hold onto an instance of TraceProcessor to track when the query
  // ends in the sql stats table. As iterators are movable, we need to null out
  // the TraceProcessor in the moved out iterator to avoid double recording
  // query ends. We could manually define a move constructor instead, but given
  // the error prone nature of keeping functions up to date, this seems like a
  // nicer approach.
  using ScopedTraceProcessor =
      base::ScopedResource<TraceProcessorImpl*, &DummyClose, nullptr>;

  void RecordFirstNextInSqlStats();

  ScopedTraceProcessor trace_processor_;
  sqlite3* db_ = nullptr;
  ScopedStmt stmt_;
  uint32_t column_count_ = 0;
  util::Status status_;

  uint32_t sql_stats_row_ = 0;
  bool called_next_ = false;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_ITERATOR_IMPL_H_
