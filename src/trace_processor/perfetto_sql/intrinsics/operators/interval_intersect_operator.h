/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_INTERVAL_INTERSECT_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_INTERVAL_INTERSECT_OPERATOR_H_

#include <sqlite3.h>
#include <memory>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/module_lifecycle_manager.h"

namespace perfetto::trace_processor {

struct IntervalIntersectOperator : sqlite::Module<IntervalIntersectOperator> {
  enum OperatorType { kInner = 0, kOuter = 1 };
  struct State {
    PerfettoSqlEngine* engine;
  };
  struct Context {
    explicit Context(PerfettoSqlEngine* _engine) : engine(_engine) {}
    PerfettoSqlEngine* engine;
    sqlite::ModuleStateManager<IntervalIntersectOperator> manager;
  };
  struct Vtab : sqlite::Module<IntervalIntersectOperator>::Vtab {
    sqlite::ModuleStateManager<IntervalIntersectOperator>::PerVtabState* state;
  };

  struct Cursor : sqlite::Module<IntervalIntersectOperator>::Cursor {
    struct InnerData {
      std::unique_ptr<IntervalTree> tree;
      std::vector<uint32_t> result_ids;
      uint32_t index = 0;

      inline uint32_t GetResultId() const { return result_ids[index]; }
      inline void Query(uint64_t start, uint64_t end) {
        result_ids.clear();
        index = 0;
        tree->FindOverlaps(start, end, result_ids);
      }
    };
    struct OuterData {
      std::unique_ptr<Table::Iterator> it;
      uint32_t ts_col_id = 0;
      uint32_t ts_end_col_id = 0;
      uint32_t id_col_id = 0;

      int64_t GetTs() { return it->Get(ts_col_id).AsLong(); }
      int64_t GetId() { return it->Get(id_col_id).AsLong(); }
      int64_t GetTsEnd() { return it->Get(ts_end_col_id).AsLong(); }
    };

    OperatorType type;
    std::string table_name;

    // Only one of those can be non null.
    InnerData inner;
    OuterData outer;
  };

  static constexpr auto kType = kEponymousOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;

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

  // This needs to happen at the end as it depends on the functions
  // defined above.
  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_INTERVAL_INTERSECT_OPERATOR_H_
