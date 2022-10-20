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
#include "perfetto/ext/base/optional.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorImpl;

class IteratorImpl {
 public:
  struct StmtMetadata {
    uint32_t column_count = 0;
    uint32_t statement_count = 0;
    uint32_t statement_count_with_output = 0;
  };

  IteratorImpl(TraceProcessorImpl* impl,
               sqlite3* db,
               base::Status,
               ScopedStmt,
               StmtMetadata,
               uint32_t sql_stats_row);
  ~IteratorImpl();

  IteratorImpl(IteratorImpl&) noexcept = delete;
  IteratorImpl& operator=(IteratorImpl&) = delete;

  IteratorImpl(IteratorImpl&&) noexcept = default;
  IteratorImpl& operator=(IteratorImpl&&) = default;

  // Methods called by the base Iterator class.
  bool Next() {
    PERFETTO_DCHECK(stmt_ || !status_.ok());

    if (!called_next_) {
      // Delegate to the cc file to prevent trace_storage.h include in this
      // file.
      RecordFirstNextInSqlStats();
      called_next_ = true;

      // In the past, we used to call sqlite3_step for the first time in this
      // function which 1:1 matched Next calls to sqlite3_step calls. However,
      // with the introduction of multi-statement support, we call
      // sqlite3_step when tokenizing the queries and so we need to *not* call
      // step the first time Next is called.
      //
      // Aside: if we could, we would change the API to match the new setup
      // (i.e. implement operator bool, make Next return nothing similar to C++
      // iterators); however, too many clients depend on the current behavior so
      // we have to keep the API as is.
      return status_.ok() && !sqlite_utils::IsStmtDone(*stmt_);
    }

    if (!status_.ok())
      return false;

    int ret = sqlite3_step(*stmt_);
    if (PERFETTO_UNLIKELY(ret != SQLITE_ROW && ret != SQLITE_DONE)) {
      status_ = base::ErrStatus("%s", sqlite_utils::FormatErrorMessage(
                                          stmt_.get(), base::nullopt, db_, ret)
                                          .c_message());
      stmt_.reset();
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
    return stmt_ ? sqlite3_column_name(*stmt_, static_cast<int>(col)) : "";
  }

  base::Status Status() { return status_; }

  uint32_t ColumnCount() { return stmt_metadata_.column_count; }

  uint32_t StatementCount() { return stmt_metadata_.statement_count; }

  uint32_t StatementCountWithOutput() {
    return stmt_metadata_.statement_count_with_output;
  }

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
  base::Status status_;

  ScopedStmt stmt_;
  StmtMetadata stmt_metadata_;

  uint32_t sql_stats_row_ = 0;
  bool called_next_ = false;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_ITERATOR_IMPL_H_
