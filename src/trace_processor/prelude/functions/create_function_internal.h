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

#ifndef SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_CREATE_FUNCTION_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_CREATE_FUNCTION_INTERNAL_H_

#include <sqlite3.h>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto {
namespace trace_processor {

struct Prototype {
  std::string function_name;
  std::vector<sql_argument::ArgumentDefinition> arguments;

  bool operator==(const Prototype& other) const {
    return function_name == other.function_name && arguments == other.arguments;
  }
  bool operator!=(const Prototype& other) const { return !(*this == other); }
};

base::Status ParseFunctionName(base::StringView raw,
                               base::StringView& function_name);

base::Status ParsePrototype(base::StringView raw, Prototype& out);

base::Status SqliteRetToStatus(sqlite3* db,
                               const std::string& function_name,
                               int ret);

base::Status MaybeBindArgument(sqlite3_stmt*,
                               const std::string& function_name,
                               const sql_argument::ArgumentDefinition&,
                               sqlite3_value*);

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PRELUDE_FUNCTIONS_CREATE_FUNCTION_INTERNAL_H_
