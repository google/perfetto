/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_STDLIB_DOCS_OPERATOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_STDLIB_DOCS_OPERATOR_H_

#include <sqlite3.h>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_module.h"
#include "src/trace_processor/util/stdlib_doc_parser.h"

namespace perfetto::trace_processor {

// Eponymous vtab: __intrinsic_stdlib_modules
// Returns all modules registered in the engine, one row per module.
struct StdlibDocsModulesOperator : sqlite::Module<StdlibDocsModulesOperator> {
  using Context = PerfettoSqlEngine;
  struct Vtab : sqlite::Module<StdlibDocsModulesOperator>::Vtab {
    const PerfettoSqlEngine* engine;
  };
  struct Cursor : sqlite::Module<StdlibDocsModulesOperator>::Cursor {
    std::vector<std::pair<std::string, std::string>> rows;  // (module, package)
    size_t index = 0;
  };
  enum Column { kModuleName = 0, kPackage };

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

  static constexpr sqlite3_module kModule = CreateModule();
};

// Eponymous vtab: __intrinsic_stdlib_tables
// Returns tables/views defined in the module given by WHERE module = '...'.
struct StdlibDocsTablesOperator : sqlite::Module<StdlibDocsTablesOperator> {
  using Context = PerfettoSqlEngine;
  struct Vtab : sqlite::Module<StdlibDocsTablesOperator>::Vtab {
    const PerfettoSqlEngine* engine;
  };
  struct Cursor : sqlite::Module<StdlibDocsTablesOperator>::Cursor {
    std::vector<stdlib_doc::TableOrView> rows;
    size_t index = 0;
  };
  enum Column {
    kName = 0,
    kSqlType,
    kDescription,
    kExposed,
    kCols,
    kModuleArg,
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

  static constexpr sqlite3_module kModule = CreateModule();
};

// Eponymous vtab: __intrinsic_stdlib_functions
// Returns functions defined in the module given by WHERE module = '...'.
struct StdlibDocsFunctionsOperator
    : sqlite::Module<StdlibDocsFunctionsOperator> {
  using Context = PerfettoSqlEngine;
  struct Vtab : sqlite::Module<StdlibDocsFunctionsOperator>::Vtab {
    const PerfettoSqlEngine* engine;
  };
  struct Cursor : sqlite::Module<StdlibDocsFunctionsOperator>::Cursor {
    std::vector<stdlib_doc::Function> rows;
    size_t index = 0;
  };
  enum Column {
    kName = 0,
    kDescription,
    kExposed,
    kIsTableFunction,
    kReturnType,
    kReturnDescription,
    kArgs,
    kCols,
    kModuleArg,
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

  static constexpr sqlite3_module kModule = CreateModule();
};

// Eponymous vtab: __intrinsic_stdlib_macros
// Returns macros defined in the module given by WHERE module = '...'.
struct StdlibDocsMacrosOperator : sqlite::Module<StdlibDocsMacrosOperator> {
  using Context = PerfettoSqlEngine;
  struct Vtab : sqlite::Module<StdlibDocsMacrosOperator>::Vtab {
    const PerfettoSqlEngine* engine;
  };
  struct Cursor : sqlite::Module<StdlibDocsMacrosOperator>::Cursor {
    std::vector<stdlib_doc::Macro> rows;
    size_t index = 0;
  };
  enum Column {
    kName = 0,
    kDescription,
    kExposed,
    kReturnType,
    kReturnDescription,
    kArgs,
    kModuleArg,
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

  static constexpr sqlite3_module kModule = CreateModule();
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_OPERATORS_STDLIB_DOCS_OPERATOR_H_
