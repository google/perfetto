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

#include <cstddef>
#include <cstdint>

#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/cursor.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {

// Adapter class between SQLite and the Dataframe API. Allows SQLite to query
// and iterate over the results of a dataframe query.
struct DataframeModule : sqlite::Module<DataframeModule> {
  using Context = void;
  static constexpr auto kType = kEponymousOnly;
  static constexpr bool kSupportsWrites = false;
  static constexpr bool kDoesOverloadFunctions = false;

  struct SqliteValueFetcher : dataframe::ValueFetcher {
    using Type = sqlite::Type;
    static const Type kInt64 = sqlite::Type::kInteger;
    static const Type kDouble = sqlite::Type::kFloat;
    static const Type kString = sqlite::Type::kText;
    static const Type kNull = sqlite::Type::kNull;

    int64_t GetInt64Value(uint32_t idx) const {
      return sqlite::value::Int64(sqlite_value[idx]);
    }
    double GetDoubleValue(uint32_t idx) const {
      return sqlite::value::Double(sqlite_value[idx]);
    }
    const char* GetStringValue(uint32_t idx) const {
      return sqlite::value::Text(sqlite_value[idx]);
    }
    Type GetValueType(uint32_t idx) const {
      return sqlite::value::Type(sqlite_value[idx]);
    }
    sqlite3_value** sqlite_value;
  };
  struct SqliteResultCallback : dataframe::CellCallback {
    void OnCell(int64_t v) const { sqlite::result::Long(ctx, v); }
    void OnCell(double v) const { sqlite::result::Double(ctx, v); }
    void OnCell(NullTermStringView v) const {
      sqlite::result::StaticString(ctx, v.data());
    }
    void OnCell(nullptr_t) const { sqlite::result::Null(ctx); }
    void OnCell(uint32_t v) const { sqlite::result::Long(ctx, v); }
    void OnCell(int32_t v) const { sqlite::result::Long(ctx, v); }
    sqlite3_context* ctx;
  };
  struct Vtab : sqlite::Module<DataframeModule>::Vtab {
    StringPool* string_pool;
    dataframe::Dataframe dataframe;
  };
  using DfCursor = dataframe::Cursor<SqliteValueFetcher>;
  struct Cursor : sqlite::Module<DataframeModule>::Cursor {
    DfCursor* df_cursor() { return reinterpret_cast<DfCursor*>(cursor); }
    alignas(DfCursor) char cursor[sizeof(DfCursor)];
    const char* last_idx_str = nullptr;
  };

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

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_
