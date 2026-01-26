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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_BUILDER_OPS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_BUILDER_OPS_H_

#include "src/trace_processor/sqlite/bindings/sqlite_function.h"

namespace perfetto::trace_processor {

// Creates a SelectOp descriptor for tree filtering.
struct TreeSelectOpFn : public sqlite::Function<TreeSelectOpFn> {
  static constexpr char kName[] = "__intrinsic_tree_select_op";
  static constexpr int kArgCount = 3;
  using UserData = void;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv);
};

// Applies a filter operation to a tree builder.
struct TreeFilterFn : public sqlite::Function<TreeFilterFn> {
  static constexpr char kName[] = "__intrinsic_tree_filter";
  static constexpr int kArgCount = 2;
  using UserData = void;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv);
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_FUNCTIONS_TREES_TREE_BUILDER_OPS_H_
