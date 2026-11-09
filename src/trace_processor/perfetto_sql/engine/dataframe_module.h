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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/engine/column_results.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/module_state_manager.h"

namespace perfetto::trace_processor {

// Adapter class between SQLite and the Dataframe API. Allows SQLite to query
// and iterate over the results of a dataframe query.
struct DataframeModule : sqlite::Module<DataframeModule> {
  static constexpr auto kType = kCreateOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;
  static constexpr bool kDoesSupportTransactions = true;

  struct State {
    explicit State(dataframe::Dataframe* _dataframe) : dataframe(_dataframe) {}
    explicit State(std::unique_ptr<dataframe::Dataframe> _owned_dataframe)
        : owned_dataframe(std::move(_owned_dataframe)),
          dataframe(owned_dataframe.get()) {}
    std::unique_ptr<dataframe::Dataframe> owned_dataframe;
    dataframe::Dataframe* dataframe;
    std::vector<std::string> named_indexes;
  };
  struct Context : sqlite::ModuleStateManager<DataframeModule> {
    explicit Context(sqlite::CommittedStateManager& store)
        : sqlite::ModuleStateManager<DataframeModule>(store) {}
    std::unique_ptr<State> temporary_create_state;
  };
  struct SqliteValueFetcher : core::ValueFetcher {
    SqliteValueFetcher(sqlite3_value** args, int count)
        : argv(args), argc(static_cast<uint32_t>(count)), values(args) {
      PERFETTO_DCHECK(argc <= in_values.size());
    }

    using Type = sqlite::Type;
    static const Type kInt64 = sqlite::Type::kInteger;
    static const Type kDouble = sqlite::Type::kFloat;
    static const Type kString = sqlite::Type::kText;
    static const Type kNull = sqlite::Type::kNull;

    int64_t GetInt64Value(uint32_t idx) const {
      return sqlite::value::Int64(values[idx]);
    }
    double GetDoubleValue(uint32_t idx) const {
      return sqlite::value::Double(values[idx]);
    }
    const char* GetStringValue(uint32_t idx) const {
      return sqlite::value::Text(values[idx]);
    }
    Type GetValueType(uint32_t idx) const {
      return sqlite::value::Type(values[idx]);
    }
    bool IteratorInit(uint32_t idx) {
      // Copied only when an IN list needs a slot for its current value.
      if (values == argv) {
        std::copy(argv, argv + argc, in_values.begin());
        values = in_values.data();
      }
      return sqlite3_vtab_in_first(argv[idx], &in_values[idx]) == SQLITE_OK;
    }
    bool IteratorNext(uint32_t idx) {
      return sqlite3_vtab_in_next(argv[idx], &in_values[idx]) == SQLITE_OK;
    }
    sqlite3_value** argv;
    uint32_t argc;
    sqlite3_value** values;
    std::array<sqlite3_value*, 16> in_values;
  };
  // Returns a dataframe cell to SQLite, for the table functions which read
  // dataframes through their own cursors.
  struct SqliteResultCallback : dataframe::CellCallback {
    void OnCell(int64_t v) const { sqlite::result::Long(ctx, v); }
    void OnCell(double v) const { sqlite::result::Double(ctx, v); }
    void OnCell(NullTermStringView v) const {
      sqlite::result::StaticTerminatedString(ctx, v.data(), v.size());
    }
    void OnCell(std::nullptr_t) const { sqlite::result::Null(ctx); }
    void OnCell(uint32_t v) const { sqlite::result::Long(ctx, v); }
    void OnCell(int32_t v) const { sqlite::result::Long(ctx, v); }
    sqlite3_context* ctx;
  };
  struct ExecPlan;
  struct Vtab : sqlite::Module<DataframeModule>::Vtab {
    sqlite::ModuleStateManager<DataframeModule>::PerVtabState* state;
    std::string name;
    int best_idx_num = 0;
    uint32_t id_col_idx = 0;
    std::unordered_map<std::string, std::shared_ptr<const ExecPlan>> exec_plans;
  };
  // Built once per table and query string, and shared by every cursor, unless
  // the table is still being added to.
  struct ExecPlan {
    static constexpr uint32_t kUnread = std::numeric_limits<uint32_t>::max();

    std::unique_ptr<pipeline::PhysicalPlan> plan;
    // By column of the table, its column in the plan's batches.
    std::vector<uint32_t> columns;
    StringPool* pool = nullptr;
    uint32_t id_index = 0;
  };

  struct ExecQuery {
    explicit ExecQuery(std::shared_ptr<const ExecPlan> p)
        : plan(std::move(p)),
          rows(plan->plan->source()),
          params(*plan->plan),
          results(plan->columns.size()) {}

    // Chosen at the first row read: a cursor's views keep their kind and type
    // from batch to batch.
    struct Result {
      ColumnResultFn fn = nullptr;
      uint32_t index = 0;
      // What `fn` was chosen for: a sort can serve some batches viewed where
      // they are held and others copied, which store a column differently.
      core::exec::ColumnView::Kind kind = core::exec::ColumnView::Kind::kFlat;
      uint32_t type = 0;
    };

    std::shared_ptr<const ExecPlan> plan;
    core::exec::RowCursor rows;
    pipeline::RunParams<SqliteValueFetcher> params;
    std::vector<Result> results;
    struct Prefetch {
      uint32_t index;
      uint32_t shift;
    };
    std::vector<Prefetch> prefetches;
    bool resolved = false;
    // Whether the plan's literals have been cast, which the first run does.
    bool literals_set = false;
  };

  struct Cursor : sqlite::Module<DataframeModule>::Cursor {
    const char* last_idx_str = nullptr;
    // In place rather than behind a pointer, saving a load per cell read.
    std::optional<ExecQuery> exec;
    bool eof = true;
  };

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
  PERFETTO_NO_INLINE static void ResolveResults(ExecQuery*);
  // Chooses the results again if a column of the current batch is stored
  // differently from the batch they were chosen for.
  static void CheckResults(ExecQuery*);
  // Out of line: it is rarely an error and the status is costly to make.
  PERFETTO_NO_INLINE static int CheckExecStatus(Cursor*);
  static int Next(sqlite3_vtab_cursor*);
  static int Eof(sqlite3_vtab_cursor*);
  static int Column(sqlite3_vtab_cursor*, sqlite3_context*, int);
  static int Rowid(sqlite3_vtab_cursor*, sqlite_int64*);

  static int Begin(sqlite3_vtab*) { return SQLITE_OK; }
  static int Sync(sqlite3_vtab*) { return SQLITE_OK; }
  static int Commit(sqlite3_vtab*) { return SQLITE_OK; }
  static int Rollback(sqlite3_vtab*) { return SQLITE_OK; }
  static int Savepoint(sqlite3_vtab* t, int r) {
    DataframeModule::Vtab* vtab = GetVtab(t);
    sqlite::ModuleStateManager<DataframeModule>::OnSavepoint(vtab->state, r);
    return SQLITE_OK;
  }
  static int Release(sqlite3_vtab* t, int r) {
    DataframeModule::Vtab* vtab = GetVtab(t);
    sqlite::ModuleStateManager<DataframeModule>::OnRelease(vtab->state, r);
    return SQLITE_OK;
  }
  static int RollbackTo(sqlite3_vtab* t, int r) {
    DataframeModule::Vtab* vtab = GetVtab(t);
    sqlite::ModuleStateManager<DataframeModule>::OnRollbackTo(vtab->state, r);
    return SQLITE_OK;
  }

  // This needs to happen at the end as it depends on the functions
  // defined above.
  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_
