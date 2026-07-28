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

#include "src/trace_processor/plugins/tree_functions/tree_conversion.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/tree_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/tree/tree_columns.h"
#include "src/trace_processor/core/tree/tree_columns_from_dataframe.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/tree_functions/tree_functions.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

namespace {

struct AggCtx : sqlite::AggregateContext<AggCtx> {
  std::optional<dataframe::AdhocDataframeBuilder> builder;
};

void PushColumnValue(dataframe::AdhocDataframeBuilder* builder,
                     uint32_t col_idx,
                     const core::tree::TreeColumns::Column& column,
                     uint32_t row) {
  const uint8_t* data = column.data.begin();
  switch (column.type.index()) {
    case core::StorageType::GetTypeIndex<core::Uint32>():
      builder->PushNonNull(
          col_idx,
          static_cast<int64_t>(reinterpret_cast<const uint32_t*>(data)[row]));
      return;
    case core::StorageType::GetTypeIndex<core::Int32>():
      builder->PushNonNull(
          col_idx,
          static_cast<int64_t>(reinterpret_cast<const int32_t*>(data)[row]));
      return;
    case core::StorageType::GetTypeIndex<core::Int64>():
      builder->PushNonNull(col_idx,
                           reinterpret_cast<const int64_t*>(data)[row]);
      return;
    case core::StorageType::GetTypeIndex<core::Double>():
      builder->PushNonNull(col_idx, reinterpret_cast<const double*>(data)[row]);
      return;
    case core::StorageType::GetTypeIndex<core::String>():
      builder->PushNonNull(col_idx,
                           reinterpret_cast<const StringPool::Id*>(data)[row]);
      return;
    default:
      PERFETTO_FATAL("Unsupported tree column type");
  }
}

base::StatusOr<dataframe::Dataframe> TreeToDataframe(
    core::tree::TreeColumns tree,
    StringPool* pool) {
  std::vector<std::string> names = {"_tree_id", "_tree_parent_id"};
  names.insert(names.end(), tree.names.begin(), tree.names.end());
  dataframe::AdhocDataframeBuilder builder(
      std::move(names), pool,
      dataframe::AdhocDataframeBuilder::Options{
          {}, dataframe::NullabilityType::kDenseNull});

  bool ok = true;
  for (uint32_t row = 0; row < tree.row_count && ok; ++row) {
    ok = builder.PushNonNull(0, row);
    if (tree.parent[row] == core::kNullParent) {
      builder.PushNull(1);
    } else {
      ok = ok && builder.PushNonNull(1, tree.parent[row]);
    }
    for (uint32_t col = 0; col < tree.columns.size(); ++col) {
      const core::tree::TreeColumns::Column& column = tree.columns[col];
      if (column.null_bv.size() > 0 && !column.null_bv.is_set(row)) {
        builder.PushNull(col + 2);
      } else {
        PushColumnValue(&builder, col + 2, column, row);
      }
    }
  }
  if (!ok) {
    return builder.status();
  }
  return std::move(builder).Build();
}

}  // namespace

void TreeFromTable::Step(sqlite3_context* ctx,
                         int rargc,
                         sqlite3_value** argv) {
  auto argc = static_cast<uint32_t>(rargc);
  auto& agg = AggCtx::GetOrCreateContextForStep(ctx);
  if (PERFETTO_UNLIKELY(!agg.builder)) {
    if (PERFETTO_UNLIKELY(argc < 4 || argc % 2 != 0)) {
      return sqlite::result::Error(
          ctx, "tree_from_table: incorrect argument layout");
    }
    uint32_t num_cols = argc / 2;
    std::vector<std::string> col_names;
    col_names.reserve(num_cols);
    for (uint32_t i = 0; i < argc; i += 2) {
      SQLITE_ASSIGN_OR_RETURN(
          ctx, auto col_name,
          sqlite::utils::ExtractArgument(argc, argv, "column name", i,
                                         SqlValue::Type::kString));
      col_names.emplace_back(col_name.AsString());
    }
    agg.builder.emplace(std::move(col_names), GetUserData(ctx),
                        dataframe::AdhocDataframeBuilder::Options{
                            {}, dataframe::NullabilityType::kDenseNull, false});
  }
  bool ok = true;
  for (uint32_t col = 0; col < argc / 2 && ok; ++col) {
    sqlite3_value* value = argv[(2 * col) + 1];
    switch (sqlite::value::Type(value)) {
      case sqlite::Type::kInteger:
        ok = agg.builder->PushNonNull(col, sqlite::value::Int64(value));
        break;
      case sqlite::Type::kFloat:
        ok = agg.builder->PushNonNull(col, sqlite::value::Double(value));
        break;
      case sqlite::Type::kText:
        ok = agg.builder->PushNonNull(
            col, GetUserData(ctx)->InternString(sqlite::value::Text(value)));
        break;
      case sqlite::Type::kNull:
        agg.builder->PushNull(col);
        break;
      case sqlite::Type::kBlob:
        return sqlite::result::Error(ctx,
                                     "tree_from_table: blobs are unsupported");
    }
  }
  if (!ok) {
    return sqlite::utils::SetError(ctx, agg.builder->status());
  }
}

void TreeFromTable::Final(sqlite3_context* ctx) {
  auto raw_agg = AggCtx::GetContextOrNullForFinal(ctx);
  if (PERFETTO_UNLIKELY(!raw_agg)) {
    return sqlite::utils::ReturnNullFromFunction(ctx);
  }
  auto& agg = *raw_agg.get();
  PERFETTO_CHECK(agg.builder);
  SQLITE_ASSIGN_OR_RETURN(
      ctx, auto cols, core::tree::BuildTreeColumns(std::move(*agg.builder)));
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<sqlite::utils::MovePointer<core::tree::TreeColumns>>(
          std::move(cols)),
      "TREE");
}

void TreeToTable::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 1) {
    return sqlite::result::Error(ctx,
                                 "tree_to_table: expected exactly 1 argument");
  }
  auto* tree_ptr = sqlite::value::Pointer<
      sqlite::utils::MovePointer<core::tree::TreeColumns>>(argv[0], "TREE");
  if (!tree_ptr) {
    return sqlite::result::Error(ctx, "tree_to_table: expected TREE");
  }
  if (tree_ptr->taken()) {
    return sqlite::result::Error(
        ctx, "tree_to_table: tree has already been consumed");
  }
  SQLITE_ASSIGN_OR_RETURN(ctx, auto df,
                          TreeToDataframe(tree_ptr->Take(), GetUserData(ctx)));
  return sqlite::result::UniquePointer(
      ctx, std::make_unique<dataframe::Dataframe>(std::move(df)), "TABLE");
}

namespace tree_functions {
namespace {

class TreeFunctionsPlugin : public Plugin<TreeFunctionsPlugin> {
 public:
  ~TreeFunctionsPlugin() override;

  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeFunctionRegistration<TreeToTable>(pool));
  }

  void RegisterAggregateFunctions(
      PerfettoSqlConnection*,
      std::vector<AggregateFunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeAggregateRegistration<TreeFromTable>(pool));
  }
};

TreeFunctionsPlugin::~TreeFunctionsPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<TreeFunctionsPlugin>();
      },
      TreeFunctionsPlugin::kPluginId, TreeFunctionsPlugin::kDepIds.data(),
      TreeFunctionsPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace tree_functions
}  // namespace perfetto::trace_processor
