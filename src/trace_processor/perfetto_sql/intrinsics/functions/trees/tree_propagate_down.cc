/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_propagate_down.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/tree/tree_transformer.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

void TreePropagateDown::Step(sqlite3_context* ctx,
                             int argc,
                             sqlite3_value** argv) {
  if (argc != 4) {
    return sqlite::result::Error(
        ctx,
        "tree_propagate_down: expected 4 arguments "
        "(tree_ptr, column_name, combine_op, output_name)");
  }

  // Extract tree pointer.
  auto* tree_ptr =
      sqlite::value::Pointer<sqlite::utils::MovePointer<tree::TreeTransformer>>(
          argv[0], "TREE_TRANSFORMER");
  if (!tree_ptr) {
    return sqlite::result::Error(ctx,
                                 "tree_propagate_down: expected "
                                 "TREE_TRANSFORMER");
  }
  if (tree_ptr->taken()) {
    return sqlite::result::Error(
        ctx, "tree_propagate_down: tree has already been consumed");
  }

  // Extract column name.
  if (sqlite::value::Type(argv[1]) != sqlite::Type::kText) {
    return sqlite::result::Error(
        ctx, "tree_propagate_down: column_name must be a string");
  }
  std::string column_name = sqlite::value::Text(argv[1]);

  // Extract combine operation.
  if (sqlite::value::Type(argv[2]) != sqlite::Type::kText) {
    return sqlite::result::Error(
        ctx, "tree_propagate_down: combine_op must be a string");
  }
  std::string combine_op = sqlite::value::Text(argv[2]);

  // Extract output name.
  if (sqlite::value::Type(argv[3]) != sqlite::Type::kText) {
    return sqlite::result::Error(
        ctx, "tree_propagate_down: output_name must be a string");
  }
  std::string output_name = sqlite::value::Text(argv[3]);

  // Take ownership of the transformer.
  auto transformer = tree_ptr->Take();

  auto status = transformer.PropagateDown(column_name, combine_op, output_name);
  SQLITE_RETURN_IF_ERROR(ctx, status);

  // Return the modified transformer wrapped in a new MovePointer.
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<sqlite::utils::MovePointer<tree::TreeTransformer>>(
          std::move(transformer)),
      "TREE_TRANSFORMER");
}

}  // namespace perfetto::trace_processor
