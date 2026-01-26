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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_UTILS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_UTILS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/tree/tree_builder.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {

// SQLite pointer wrapper for TreeTransformationBuilder.
// Tracks whether the builder has been consumed to prevent double-use.
struct TreeBuilderWrapper {
  tree::TreeTransformationBuilder builder;
  bool consumed = false;

  explicit TreeBuilderWrapper(std::unique_ptr<tree::Tree> t)
      : builder(std::move(t)) {}

  TreeBuilderWrapper(TreeBuilderWrapper&&) = delete;
  TreeBuilderWrapper& operator=(TreeBuilderWrapper&&) = delete;
  TreeBuilderWrapper(const TreeBuilderWrapper&) = delete;
  TreeBuilderWrapper& operator=(const TreeBuilderWrapper&) = delete;

  tree::TreeTransformationBuilder Take() {
    consumed = true;
    return std::move(builder);
  }

  bool WasTaken() const { return consumed; }
};

// Describes a filter/select operation for tree nodes.
struct SelectOp {
  using Op = tree::TreeTransformationBuilder::FilterOp;
  using Value = tree::TreeTransformationBuilder::FilterValue;

  std::string column_name;
  Op op;
  Value value;
};

// Parses operator string ("=", "!=", "<", etc.) to Op.
std::optional<SelectOp::Op> ParseSelectOpOperator(const char* op_str);

// Extracts value from sqlite3_value for use in SelectOp.
std::optional<std::variant<int64_t, double, std::string>> ExtractSelectOpValue(
    sqlite3_value* arg);

// SQLite argument extraction helpers with error reporting.
base::StatusOr<const char*> GetTextArg(sqlite3_value* arg,
                                       const char* arg_name);
base::StatusOr<int64_t> GetInt64Arg(sqlite3_value* arg, const char* arg_name);
base::StatusOr<std::optional<int64_t>> GetOptionalInt64Arg(
    sqlite3_value* arg,
    const char* arg_name);

// SQLite pointer extraction helpers.
base::StatusOr<TreeBuilderWrapper*> GetTreeBuilder(sqlite3_value* arg);
base::StatusOr<SelectOp*> GetSelectOp(sqlite3_value* arg);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_UTILS_H_
