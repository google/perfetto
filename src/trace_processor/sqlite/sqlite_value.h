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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQLITE_VALUE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQLITE_VALUE_H_

#include <sqlite3.h>
#include <cstdint>

struct sqlite_value;

namespace perfetto::trace_processor::sqlite::value {

// This file contains thin wrappers around the sqlite3_value_* functions which
// fetches data from SQLite in fuction definitions, virtual table filter clauses
// etc.

inline int64_t Long(sqlite3_value* value) {
  return sqlite3_value_int64(value);
}

inline bool IsNull(sqlite3_value* value) {
  return sqlite3_value_type(value) == SQLITE_NULL;
}

}  // namespace perfetto::trace_processor::sqlite::value

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_VALUE_H_
