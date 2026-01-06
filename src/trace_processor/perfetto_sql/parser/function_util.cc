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

#include "src/trace_processor/perfetto_sql/parser/function_util.h"

#include <sqlite3.h>
#include <cstdint>
#include <string>

#include "perfetto/base/status.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor {

base::Status SqliteRetToStatus(sqlite3* db,
                               const std::string& function_name,
                               int ret) {
  if (ret != SQLITE_ROW && ret != SQLITE_DONE) {
    return base::ErrStatus("%s: SQLite error while executing function body: %s",
                           function_name.c_str(), sqlite3_errmsg(db));
  }
  return base::OkStatus();
}

base::Status MaybeBindArgument(sqlite3_stmt* stmt,
                               const std::string& function_name,
                               const sql_argument::ArgumentDefinition& arg,
                               sqlite3_value* value) {
  int index = sqlite3_bind_parameter_index(stmt, arg.dollar_name().c_str());

  // If the argument is not in the query, this just means its an unused
  // argument which we can just ignore.
  if (index == 0)
    return base::Status();

  int ret = sqlite3_bind_value(stmt, index, value);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "%s: SQLite error while binding value to argument %s: %s",
        function_name.c_str(), arg.name().c_str(),
        sqlite3_errmsg(sqlite3_db_handle(stmt)));
  }
  return base::OkStatus();
}

base::Status MaybeBindIntArgument(sqlite3_stmt* stmt,
                                  const std::string& function_name,
                                  const sql_argument::ArgumentDefinition& arg,
                                  int64_t value) {
  int index = sqlite3_bind_parameter_index(stmt, arg.dollar_name().c_str());

  // If the argument is not in the query, this just means its an unused
  // argument which we can just ignore.
  if (index == 0)
    return base::Status();

  int ret = sqlite3_bind_int64(stmt, index, value);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "%s: SQLite error while binding value to argument %s: %s",
        function_name.c_str(), arg.name().c_str(),
        sqlite3_errmsg(sqlite3_db_handle(stmt)));
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
