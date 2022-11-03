/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/sqlite/functions/import.h"

#include <numeric>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/functions/create_function_internal.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

base::Status Import::Run(Import::Context* ctx,
                         size_t argc,
                         sqlite3_value** argv,
                         SqlValue&,
                         Destructors&) {
  if (argc != 1) {
    return base::ErrStatus(
        "IMPORT: invalid number of args; expected 1, received "
        "%zu",
        argc);
  }
  sqlite3_value* path_val = argv[0];

  // Type check
  {
    base::Status status =
        TypeCheckSqliteValue(path_val, SqlValue::Type::kString);
    if (!status.ok()) {
      return base::ErrStatus("IMPORT(%s): %s", sqlite3_value_text(path_val),
                             status.c_message());
    }
  }

  const char* path =
      reinterpret_cast<const char*>(sqlite3_value_text(path_val));

  auto lib_file = ctx->path_to_lib_file.Find(std::string(path));
  if (!lib_file) {
    return base::ErrStatus("IMPORT: Unknown filename provided - %s", path);
  }
  // IMPORT is noop for already imported files.
  if (lib_file->imported) {
    return base::OkStatus();
  }

  auto import_iter = ctx->tp->ExecuteQuery(lib_file->sql);
  bool import_has_more = import_iter.Next();
  if (import_has_more)
    return base::ErrStatus("IMPORT: Imported file returning values.");
  {
    auto status = import_iter.Status();
    if (!status.ok())
      return base::ErrStatus("SQLite error on IMPORT: %s", status.c_message());
  }

  lib_file->imported = true;
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
