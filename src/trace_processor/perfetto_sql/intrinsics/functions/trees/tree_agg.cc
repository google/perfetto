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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_agg.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/trees/tree_utils.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {

namespace {

struct SqliteArgvFetcher : core::ValueFetcher {
  using Type = sqlite::Type;
  [[maybe_unused]] static constexpr Type kInt64 = sqlite::Type::kInteger;
  [[maybe_unused]] static constexpr Type kDouble = sqlite::Type::kFloat;
  [[maybe_unused]] static constexpr Type kString = sqlite::Type::kText;
  [[maybe_unused]] static constexpr Type kNull = sqlite::Type::kNull;
  [[maybe_unused]] static constexpr Type kBytes = sqlite::Type::kBlob;

  [[maybe_unused]] Type GetValueType(uint32_t index) const {
    return sqlite::value::Type(argv[(index * 2) + 1]);
  }
  [[maybe_unused]] int64_t GetInt64Value(uint32_t index) const {
    return sqlite::value::Int64(argv[(index * 2) + 1]);
  }
  [[maybe_unused]] double GetDoubleValue(uint32_t index) const {
    return sqlite::value::Double(argv[(index * 2) + 1]);
  }
  [[maybe_unused]] const char* GetStringValue(uint32_t index) const {
    return sqlite::value::Text(argv[(index * 2) + 1]);
  }

  sqlite3_value** argv = nullptr;
  uint32_t num_cols = 0;
};

}  // namespace

void TreeAgg::Step(sqlite3_context* ctx, int rargc, sqlite3_value** argv) {
  auto argc = static_cast<uint32_t>(rargc);
  if (argc < 4) {
    return sqlite::result::Error(ctx,
                                 "tree_agg: need at least id and parent_id");
  }
  if (argc % 2 != 0) {
    return sqlite::result::Error(ctx,
                                 "tree_agg: must have pairs of (name, value)");
  }

  auto& agg = AggCtx::GetOrCreateContextForStep(ctx);
  uint32_t num_cols = argc / 2;

  if (!agg.builder) {
    std::vector<std::string> column_names;
    for (uint32_t i = 0; i < argc; i += 2) {
      SQLITE_ASSIGN_OR_RETURN(ctx, auto col_name,
                              GetTextArg(argv[i], "column name"));
      column_names.emplace_back(col_name);
    }
    // Use DenseNull for all columns so they support random access via GetCell.
    // This is needed because tree columns are accessed via GetCell in
    // TreeToDataframe.
    using NullabilityType = dataframe::RuntimeDataframeBuilder::NullabilityType;
    using ColumnType = dataframe::AdhocDataframeBuilder::ColumnType;
    std::vector<NullabilityType> nullability_types(column_names.size(),
                                                   NullabilityType::kDense);
    agg.builder = std::make_unique<dataframe::RuntimeDataframeBuilder>(
        column_names, GetUserData(ctx), std::vector<ColumnType>{},
        nullability_types);
  }

  SQLITE_ASSIGN_OR_RETURN(ctx, auto id, GetInt64Arg(argv[1], "id"));
  agg.id_values.push_back(id);

  SQLITE_ASSIGN_OR_RETURN(ctx, auto parent_id,
                          GetOptionalInt64Arg(argv[3], "parent_id"));
  agg.parent_id_values.push_back(parent_id.value_or(AggCtx::kNullParentId));

  SqliteArgvFetcher fetcher{{}, argv, num_cols};
  if (!agg.builder->AddRow(&fetcher)) {
    return sqlite::utils::SetError(ctx, agg.builder->status());
  }
}

void TreeAgg::Final(sqlite3_context* ctx) {
  auto raw_agg = AggCtx::GetContextOrNullForFinal(ctx);
  if (!raw_agg || !raw_agg.get()->builder) {
    return sqlite::result::Null(ctx);
  }

  auto& agg = *raw_agg.get();
  SQLITE_ASSIGN_OR_RETURN(ctx, auto df, std::move(*agg.builder).Build());

  auto num_rows = static_cast<uint32_t>(agg.id_values.size());

  base::FlatHashMap<int64_t, uint32_t> id_to_index;
  for (uint32_t i = 0; i < num_rows; ++i) {
    id_to_index[agg.id_values[i]] = i;
  }

  auto tree_ptr = std::make_unique<tree::Tree>();
  tree_ptr->parents = core::Slab<uint32_t>::Alloc(num_rows);

  for (uint32_t i = 0; i < num_rows; ++i) {
    if (agg.parent_id_values[i] == AggCtx::kNullParentId) {
      tree_ptr->parents[i] = tree::Tree::kNoParent;
    } else {
      auto* it = id_to_index.Find(agg.parent_id_values[i]);
      if (!it) {
        return sqlite::result::Error(ctx,
                                     "tree_agg: parent_id not found in ids");
      }
      tree_ptr->parents[i] = *it;
    }
  }

  tree_ptr->columns = std::move(df);

  auto wrapper = std::make_unique<TreeBuilderWrapper>(std::move(tree_ptr));
  return sqlite::result::UniquePointer(ctx, std::move(wrapper), "TREE_BUILDER");
}

}  // namespace perfetto::trace_processor
