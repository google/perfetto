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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_from_table.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

void TreeFromTable::Step(sqlite3_context* ctx,
                         int rargc,
                         sqlite3_value** argv) {
  auto argc = static_cast<uint32_t>(rargc);

  // Validate minimum argument count: need at least id and parent_id.
  if (argc < 4) {
    return sqlite::result::Error(
        ctx, "tree_from_table: need at least id and parent_id");
  }

  // Arguments must come in pairs of (name, value).
  if (argc % 2 != 0) {
    return sqlite::result::Error(
        ctx, "tree_from_table: must have pairs of (name, value)");
  }

  auto& agg = AggCtx::GetOrCreateContextForStep(ctx);

  // Total number of columns (id, parent_id, and data columns).
  uint32_t num_cols = argc / 2;

  // Extract column names on first row.
  // argv layout: [id_name, id_value, parent_id_name, parent_id_value,
  //               col0_name, col0_value, col1_name, col1_value, ...]
  std::vector<std::string> col_names;
  col_names.reserve(num_cols);
  for (uint32_t i = 0; i < argc; i += 2) {
    SQLITE_ASSIGN_OR_RETURN(
        ctx, auto col_name,
        sqlite::utils::ExtractArgument(argc, argv, "column name", i,
                                       SqlValue::Type::kString));
    col_names.emplace_back(col_name.AsString());
  }

  // TODO: Create TreeBuilder and add rows once core/tree is implemented.
  // For now, just validate the arguments.
  base::ignore_result(agg);
  base::ignore_result(col_names);
}

void TreeFromTable::Final(sqlite3_context* ctx) {
  auto raw_agg = AggCtx::GetContextOrNullForFinal(ctx);
  if (!raw_agg) {
    return sqlite::result::Null(ctx);
  }

  // TODO: Seal the builder and return as opaque pointer once core/tree is
  // implemented.
  return sqlite::result::Null(ctx);
}

}  // namespace perfetto::trace_processor
