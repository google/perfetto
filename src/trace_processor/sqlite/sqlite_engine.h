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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_

#include <sqlite3.h>

#include "src/trace_processor/db/table.h"
#include "src/trace_processor/prelude/table_functions/table_function.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"

namespace perfetto {
namespace trace_processor {

// Wrapper class around SQLite C API.
//
// The goal of this class is to provide a one-stop-shop mechanism to use SQLite.
// Benefits of this include:
// 1) It allows us to add code which intercepts registration of functions
//    and tables and keeps track of this for later lookup.
// 2) Allows easily auditing the SQLite APIs we use making it easy to determine
//    what functionality we rely on.
class SqliteEngine {
 public:
  SqliteEngine();

  // Registers a trace processor C++ table with SQLite with an SQL name of
  // |name|.
  void RegisterTable(const Table& table, const std::string& name);

  // Registers a trace processor C++ function with SQLite.
  void RegisterTableFunction(std::unique_ptr<TableFunction> fn);

  sqlite3* db() const { return db_.get(); }

 private:
  // Keep this first: we need this to be destroyed after we clean up
  // everything else.
  ScopedDb db_;
  std::unique_ptr<QueryCache> query_cache_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_ENGINE_H_
