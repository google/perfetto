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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_WINDOW_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_WINDOW_OPERATOR_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/module_state_manager.h"

namespace perfetto::trace_processor {

class TraceStorage;

// Operator table which can emit spans of a configurable duration.
struct WindowOperatorModule : sqlite::Module<WindowOperatorModule> {
  // Defines the data to be generated by the table.
  enum FilterType {
    // Returns all the spans.
    kReturnAll = 0,
    // Only returns the first span of the table. Useful for UPDATE operations.
    kReturnFirst = 1,
  };
  struct State {
    int64_t quantum = 0;
    int64_t window_start = 0;

    // max of int64_t because SQLite technically only supports int64s and not
    // uint64s.
    int64_t window_dur = std::numeric_limits<int64_t>::max();
  };
  struct Context : sqlite::ModuleStateManager<WindowOperatorModule> {};
  struct Vtab : sqlite::Module<WindowOperatorModule>::Vtab {
    sqlite::ModuleStateManager<WindowOperatorModule>::PerVtabState* state;
  };
  struct Cursor : sqlite::Module<WindowOperatorModule>::Cursor {
    int64_t window_end = 0;
    int64_t step_size = 0;

    int64_t current_ts = 0;
    int64_t quantum_ts = 0;
    int64_t row_id = 0;

    FilterType filter_type = FilterType::kReturnAll;
  };

  static constexpr auto kType = kCreateOnly;
  static constexpr bool kDoesOverloadFunctions = false;

  static int Create(sqlite3*,
                    void*,
                    int,
                    const char* const*,
                    sqlite3_vtab**,
                    char**);
  static int Destroy(sqlite3_vtab*);

  static int Connect(sqlite3*,
                     void*,
                     int,
                     const char* const*,
                     sqlite3_vtab**,
                     char**);
  static int Disconnect(sqlite3_vtab*);

  static int BestIndex(sqlite3_vtab*, sqlite3_index_info*);

  static int Open(sqlite3_vtab*, sqlite3_vtab_cursor**);
  static int Close(sqlite3_vtab_cursor*);

  static int Filter(sqlite3_vtab_cursor*,
                    int,
                    const char*,
                    int,
                    sqlite3_value**);
  static int Next(sqlite3_vtab_cursor*);
  static int Eof(sqlite3_vtab_cursor*);
  static int Column(sqlite3_vtab_cursor*, sqlite3_context*, int);
  static int Rowid(sqlite3_vtab_cursor*, sqlite_int64*);

  static int Update(sqlite3_vtab*, int, sqlite3_value**, sqlite_int64*);

  // This needs to happen at the end as it depends on the functions
  // defined above.
  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_WINDOW_OPERATOR_H_
