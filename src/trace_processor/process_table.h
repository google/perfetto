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

#ifndef SRC_TRACE_PROCESSOR_PROCESS_TABLE_H_
#define SRC_TRACE_PROCESSOR_PROCESS_TABLE_H_

#include <limits>
#include <memory>

#include "sqlite3.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// The implementation of the SQLite table containing each unique process with
// their details (only name at the moment).
class ProcessTable {
 public:
  enum Column { kUpid = 0, kName = 1 };

  ProcessTable(const TraceStorage*);
  static sqlite3_module CreateModule();

  // Implementation for sqlite3_vtab.
  int BestIndex(sqlite3_index_info*);
  int Open(sqlite3_vtab_cursor**);

 private:
  using Constraint = sqlite3_index_info::sqlite3_index_constraint;
  using OrderBy = sqlite3_index_info::sqlite3_index_orderby;

  class Cursor {
   public:
    Cursor(const TraceStorage*);

    // Implementation of sqlite3_vtab_cursor.
    int Filter(int idxNum, const char* idxStr, int argc, sqlite3_value** argv);
    int Next();
    int Eof();

    int Column(sqlite3_context* context, int N);
    int RowId(sqlite_int64* rowId);

   private:
    sqlite3_vtab_cursor base_;  // Must be first.

    const TraceStorage* const storage_;
    TraceStorage::UniquePid min_upid_;
    TraceStorage::UniquePid max_upid_;
    TraceStorage::UniquePid current_upid_;
    bool desc_ = false;
  };

  static inline Cursor* AsCursor(sqlite3_vtab_cursor* cursor) {
    return reinterpret_cast<Cursor*>(cursor);
  }

  sqlite3_vtab base_;  // Must be first.
  const TraceStorage* const storage_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROCESS_TABLE_H_
