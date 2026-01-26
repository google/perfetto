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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_builder_ops.h"

#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_utils.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

void TreeSelectOpFn::Step(sqlite3_context* ctx,
                          int argc,
                          sqlite3_value** argv) {
  PERFETTO_DCHECK(argc == kArgCount);

  SQLITE_ASSIGN_OR_RETURN(ctx, auto col_name,
                          GetTextArg(argv[0], "column name"));
  SQLITE_ASSIGN_OR_RETURN(ctx, auto op_str, GetTextArg(argv[1], "operator"));

  auto op = ParseSelectOpOperator(op_str);
  if (!op) {
    return sqlite::result::Error(
        ctx, "tree_select_op: invalid operator (use =, !=, <, <=, >, >=)");
  }

  auto value = ExtractSelectOpValue(argv[2]);
  if (!value) {
    return sqlite::result::Error(
        ctx, "tree_select_op: value must be int, float, or string");
  }

  auto select_op =
      std::unique_ptr<SelectOp>(new SelectOp{col_name, *op, std::move(*value)});

  return sqlite::result::UniquePointer(ctx, std::move(select_op), "SELECT_OP");
}

void TreeFilterFn::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  PERFETTO_DCHECK(argc == kArgCount);

  SQLITE_ASSIGN_OR_RETURN(ctx, auto wrapper, GetTreeBuilder(argv[0]));
  SQLITE_ASSIGN_OR_RETURN(ctx, auto select_op, GetSelectOp(argv[1]));

  auto builder = wrapper->Take();

  if (!builder.Filter(select_op->column_name, select_op->op,
                      select_op->value)) {
    return sqlite::result::Error(ctx, "tree_filter: column not found");
  }

  auto tree = std::move(builder).Build();
  auto new_wrapper = std::make_unique<TreeBuilderWrapper>(std::move(tree));
  return sqlite::result::UniquePointer(ctx, std::move(new_wrapper),
                                       "TREE_BUILDER");
}

}  // namespace perfetto::trace_processor
