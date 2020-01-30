/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_RAW_TABLE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_RAW_TABLE_H_

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_writer.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/trace_storage.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto {
namespace trace_processor {

class SqliteRawTable : public DbSqliteTable {
 public:
  struct Context {
    QueryCache* cache;
    const TraceStorage* storage;
  };

  SqliteRawTable(sqlite3*, Context);
  virtual ~SqliteRawTable();

  static void RegisterTable(sqlite3* db, QueryCache*, const TraceStorage*);

 private:
  void FormatSystraceArgs(NullTermStringView event_name,
                          ArgSetId arg_set_id,
                          base::StringWriter* writer);
  void ToSystrace(sqlite3_context* ctx, int argc, sqlite3_value** argv);
  bool ParseGfpFlags(Variadic value, base::StringWriter* writer);

  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_RAW_TABLE_H_
