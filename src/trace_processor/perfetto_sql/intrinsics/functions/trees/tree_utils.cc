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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_utils.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <variant>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {

std::optional<SelectOp::Op> ParseSelectOpOperator(const char* op_str) {
  if (strcmp(op_str, "=") == 0)
    return SelectOp::Op(core::Eq{});
  if (strcmp(op_str, "!=") == 0)
    return SelectOp::Op(core::Ne{});
  if (strcmp(op_str, "<") == 0)
    return SelectOp::Op(core::Lt{});
  if (strcmp(op_str, "<=") == 0)
    return SelectOp::Op(core::Le{});
  if (strcmp(op_str, ">") == 0)
    return SelectOp::Op(core::Gt{});
  if (strcmp(op_str, ">=") == 0)
    return SelectOp::Op(core::Ge{});
  return std::nullopt;
}

std::optional<std::variant<int64_t, double, std::string>> ExtractSelectOpValue(
    sqlite3_value* arg) {
  switch (sqlite::value::Type(arg)) {
    case sqlite::Type::kInteger:
      return sqlite::value::Int64(arg);
    case sqlite::Type::kFloat:
      return sqlite::value::Double(arg);
    case sqlite::Type::kText:
      return std::string(sqlite::value::Text(arg));
    case sqlite::Type::kNull:
    case sqlite::Type::kBlob:
      return std::nullopt;
  }
  PERFETTO_FATAL("Unexpected SQLite type");
}

base::StatusOr<const char*> GetTextArg(sqlite3_value* arg,
                                       const char* arg_name) {
  if (sqlite::value::Type(arg) != sqlite::Type::kText) {
    return base::ErrStatus("%s must be a string", arg_name);
  }
  return sqlite::value::Text(arg);
}

base::StatusOr<int64_t> GetInt64Arg(sqlite3_value* arg, const char* arg_name) {
  if (sqlite::value::Type(arg) != sqlite::Type::kInteger) {
    return base::ErrStatus("%s must be an integer", arg_name);
  }
  return sqlite::value::Int64(arg);
}

base::StatusOr<std::optional<int64_t>> GetOptionalInt64Arg(
    sqlite3_value* arg,
    const char* arg_name) {
  if (sqlite::value::Type(arg) == sqlite::Type::kNull) {
    return std::optional<int64_t>{};
  }
  if (sqlite::value::Type(arg) != sqlite::Type::kInteger) {
    return base::ErrStatus("%s must be an integer or null", arg_name);
  }
  return std::optional<int64_t>{sqlite::value::Int64(arg)};
}

base::StatusOr<TreeBuilderWrapper*> GetTreeBuilder(sqlite3_value* arg) {
  auto* wrapper =
      sqlite::value::Pointer<TreeBuilderWrapper>(arg, "TREE_BUILDER");
  if (!wrapper) {
    return base::ErrStatus("expected TREE_BUILDER pointer");
  }
  if (wrapper->WasTaken()) {
    return base::ErrStatus("TREE_BUILDER was already consumed");
  }
  return wrapper;
}

base::StatusOr<SelectOp*> GetSelectOp(sqlite3_value* arg) {
  auto* op = sqlite::value::Pointer<SelectOp>(arg, "SELECT_OP");
  if (!op) {
    return base::ErrStatus("expected SELECT_OP pointer");
  }
  return op;
}

}  // namespace perfetto::trace_processor
