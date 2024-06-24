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
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/ext/base/hash.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/module_lifecycle_manager.h"

namespace perfetto::trace_processor {

struct IntervalIntersectOperator : sqlite::Module<IntervalIntersectOperator> {
  static constexpr uint16_t kSchemaColumnsCount = 16;
  using SchemaCol = uint16_t;
  using SchemaToTableColumnMap =
      std::array<std::optional<SchemaCol>, kSchemaColumnsCount>;

  enum OperatorType { kInner = 0, kOuter = 1 };
  struct State {
    PerfettoSqlEngine* engine;
    std::array<std::optional<uint16_t>, kSchemaColumnsCount> argv_to_col_map{};
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
    using TreesKey = uint64_t;
    using TreesMap = base::FlatHashMap<TreesKey,
                                       std::unique_ptr<IntervalTree>,
                                       base::AlreadyHashed<TreesKey>>;

    struct InnerData {
      TreesMap trees;
      SchemaToTableColumnMap additional_cols;

      std::vector<uint32_t> query_results;
      uint32_t index = 0;

      inline uint32_t GetResultId() const { return query_results[index]; }
      inline void Query(uint64_t start,
                        uint64_t end,
                        const TreesKey& tree_key) {
        query_results.clear();
        index = 0;
        auto* tree_ptr = trees.Find(tree_key);
        if (!tree_ptr) {
          return;
        }
        (*tree_ptr)->FindOverlaps(start, end, query_results);
      }
    };

    struct OuterData {
      std::unique_ptr<Table::Iterator> it;
      SchemaToTableColumnMap additional_cols;

      inline SqlValue Get(int col) {
        return it->Get(*additional_cols[static_cast<size_t>(col)]);
      }
    };

    OperatorType type;
    std::string table_name;
    std::string exposed_cols_str;
    const Table* table = nullptr;

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
