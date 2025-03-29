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

#ifndef THIRD_PARTY_PERFETTO_SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_
#define THIRD_PARTY_PERFETTO_SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"

namespace perfetto::trace_processor {

struct DataframeModule : sqlite::Module<DataframeModule> {
  using Context = void;
  struct Vtab : sqlite::Module<DataframeModule>::Vtab {
    StringPool* string_pool;
    dataframe::Dataframe dataframe;
  };
  struct Cursor : sqlite::Module<DataframeModule>::Cursor {
    dataframe::Dataframe::Cursor* df_cursor() {
      return reinterpret_cast<dataframe::Dataframe::Cursor*>(cursor);
    }
    alignas(dataframe::Dataframe::Cursor) char cursor[sizeof(
        dataframe::Dataframe::Cursor)];
    const char* last_idx_str = nullptr;
  };

  static constexpr auto kType = kEponymousOnly;
  static constexpr bool kSupportsWrites = false;

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

  static int FindFunction(sqlite3_vtab*,
                          int,
                          const char*,
                          FindFunctionFn**,
                          void**);

  // This needs to happen at the end as it depends on the functions
  // defined above.
  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor::perfetto_sql

#endif  // THIRD_PARTY_PERFETTO_SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_MODULE_H_
