/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_BINDINGS_SQLITE_DB_H_
#define SRC_TRACE_PROCESSOR_SQLITE_BINDINGS_SQLITE_DB_H_

#include <sqlite3.h>  // IWYU pragma: export

#include "perfetto/base/compiler.h"

namespace perfetto::trace_processor::sqlite::db {

// This file wraps the SQLite functions which operate on sqlite3 (database
// connection) objects.

// Whether SQLite has progress handlers. Chromium's SQLite is built without
// them (SQLITE_OMIT_PROGRESS_CALLBACK).
#if defined(SQLITE_OMIT_PROGRESS_CALLBACK)
inline constexpr bool kHasProgressHandler = false;
#else
inline constexpr bool kHasProgressHandler = true;
#endif

// Does nothing if SQLite has no progress handlers (see kHasProgressHandler).
inline void ProgressHandler(sqlite3* db,
                            int steps,
                            int (*callback)(void*),
                            void* ctx) {
#if defined(SQLITE_OMIT_PROGRESS_CALLBACK)
  base::ignore_result(db, steps, callback, ctx);
#else
  sqlite3_progress_handler(db, steps, callback, ctx);
#endif
}

inline void Interrupt(sqlite3* db) {
  sqlite3_interrupt(db);
}

}  // namespace perfetto::trace_processor::sqlite::db

#endif  // SRC_TRACE_PROCESSOR_SQLITE_BINDINGS_SQLITE_DB_H_
