/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/type_builders.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/array.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/node.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/row_dataframe.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/struct.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace {

using Array = std::variant<perfetto_sql::IntArray,
                           perfetto_sql::DoubleArray,
                           perfetto_sql::StringArray>;

// An SQL aggregate-function which creates an array.
struct ArrayAgg : public SqliteAggregateFunction<ArrayAgg> {
  static constexpr char kName[] = "__intrinsic_array_agg";
  static constexpr int kArgCount = 1;
  struct AggCtx : SqliteAggregateContext<AggCtx> {
    template <typename T>
    void Push(sqlite3_context* ctx, T value) {
      if (PERFETTO_UNLIKELY(!array)) {
        array = std::vector<T>{std::move(value)};
        return;
      }
      auto* a = std::get_if<std::vector<T>>(&*array);
      if (!a) {
        return sqlite::result::Error(
            ctx, "ARRAY_AGG: all values must have the same type");
      }
      a->emplace_back(std::move(value));
    }
    template <typename T>
    void Result(sqlite3_context* ctx, const char* type) {
      auto res = std::make_unique<std::vector<T>>(
          std::get<std::vector<T>>(std::move(*array)));
      return sqlite::result::UniquePointer(ctx, std::move(res), type);
    }

    std::optional<Array> array;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);

    auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
    switch (sqlite::value::Type(argv[0])) {
      case sqlite::Type::kInteger:
        return agg_ctx.Push(ctx, sqlite::value::Int64(argv[0]));
      case sqlite::Type::kText:
        return agg_ctx.Push<std::string>(ctx, sqlite::value::Text(argv[0]));
      case sqlite::Type::kFloat:
        return agg_ctx.Push(ctx, sqlite::value::Double(argv[0]));
      case sqlite::Type::kNull:
        return sqlite::result::Error(
            ctx,
            "ARRAY_AGG: nulls are not supported. They should be filtered out "
            "before calling ARRAY_AGG.");
      case sqlite::Type::kBlob:
        return sqlite::result::Error(ctx,
                                     "ARRAY_AGG: blobs are not supported.");
    }
  }
  static void Final(sqlite3_context* ctx) {
    auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
    if (!raw_agg_ctx) {
      return sqlite::result::Null(ctx);
    }

    auto& array = *raw_agg_ctx.get()->array;
    switch (array.index()) {
      case 0 /* int64_t */:
        return raw_agg_ctx.get()->Result<int64_t>(ctx, "ARRAY<LONG>");
      case 1 /* double */:
        return raw_agg_ctx.get()->Result<double>(ctx, "ARRAY<DOUBLE>");
      case 2 /* std::string */:
        return raw_agg_ctx.get()->Result<std::string>(ctx, "ARRAY<STRING>");
      default:
        PERFETTO_FATAL("%zu is not a valid index", array.index());
    }
  }
};

// An SQL aggregate function which creates a graph.
struct NodeAgg : public SqliteAggregateFunction<NodeAgg> {
  static constexpr char kName[] = "__intrinsic_graph_agg";
  static constexpr int kArgCount = 2;
  struct AggCtx : SqliteAggregateContext<AggCtx> {
    perfetto_sql::Graph graph;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);

    auto source_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
    auto target_id = static_cast<uint32_t>(sqlite::value::Int64(argv[1]));
    uint32_t max_id = std::max(source_id, target_id);
    auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
    if (max_id >= agg_ctx.graph.size()) {
      agg_ctx.graph.resize(max_id + 1);
    }
    agg_ctx.graph[source_id].outgoing_edges.push_back(target_id);
  }
  static void Final(sqlite3_context* ctx) {
    auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
    if (!raw_agg_ctx.get()) {
      return;
    }
    auto nodes = std::make_unique<perfetto_sql::Graph>(
        std::move(raw_agg_ctx.get()->graph));
    return sqlite::result::UniquePointer(ctx, std::move(nodes), "GRAPH");
  }
};

// An SQL scalar function which creates an struct.
struct Struct : public SqliteFunction<Struct> {
  static constexpr char kName[] = "__intrinsic_struct";
  static constexpr int kArgCount = -1;

  static void Step(sqlite3_context* ctx, int rargc, sqlite3_value** argv) {
    auto argc = static_cast<uint32_t>(rargc);
    if (argc % 2 != 0) {
      return sqlite::result::Error(
          ctx, "STRUCT: must have an even number of arguments");
    }
    if (argc / 2 > perfetto_sql::Struct::kMaxFields) {
      return sqlite::utils::SetError(
          ctx, base::ErrStatus("STRUCT: only at most %d fields are supported",
                               perfetto_sql::Struct::kMaxFields));
    }

    auto s = std::make_unique<perfetto_sql::Struct>();
    s->field_count = argc / 2;
    for (uint32_t i = 0; i < s->field_count; ++i) {
      if (sqlite::value::Type(argv[i]) != sqlite::Type::kText) {
        return sqlite::result::Error(ctx,
                                     "STRUCT: field names must be strings");
      }
      auto& field = s->fields[i];
      field.first = sqlite::value::Text(argv[i]);
      switch (sqlite::value::Type(argv[s->field_count + i])) {
        case sqlite::Type::kText:
          field.second = sqlite::value::Text(argv[s->field_count + i]);
          break;
        case sqlite::Type::kInteger:
          field.second = sqlite::value::Int64(argv[s->field_count + i]);
          break;
        case sqlite::Type::kFloat:
          field.second = sqlite::value::Double(argv[s->field_count + i]);
          break;
        case sqlite::Type::kNull:
          field.second = std::monostate();
          break;
        case sqlite::Type::kBlob:
          return sqlite::result::Error(ctx,
                                       "STRUCT: blob fields not supported");
      }
    }
    return sqlite::result::UniquePointer(ctx, std::move(s), "STRUCT");
  }
};

// An SQL aggregate function which creates a RowDataframe.
struct RowDataframeAgg : public SqliteAggregateFunction<Struct> {
  static constexpr char kName[] = "__intrinsic_row_dataframe_agg";
  static constexpr int kArgCount = -1;
  struct AggCtx : SqliteAggregateContext<AggCtx> {
    perfetto_sql::RowDataframe dataframe;
    std::optional<uint32_t> argc_index;
  };

  static void Step(sqlite3_context* ctx, int rargc, sqlite3_value** argv) {
    auto argc = static_cast<uint32_t>(rargc);
    if (argc % 2 != 0) {
      return sqlite::result::Error(
          ctx, "ROW_DATAFRAME_AGG: must have an even number of arguments");
    }

    auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
    auto& df = agg_ctx.dataframe;
    if (df.column_names.empty()) {
      for (uint32_t i = 0; i < argc; i += 2) {
        df.column_names.emplace_back(sqlite::value::Text(argv[i]));
        if (df.column_names.back() == "id") {
          df.id_column_index = i / 2;
          agg_ctx.argc_index = i + 1;
        }
      }
    }

    if (agg_ctx.argc_index) {
      auto id = static_cast<uint32_t>(
          sqlite::value::Int64(argv[*agg_ctx.argc_index]));
      if (id >= df.id_to_cell_index.size()) {
        df.id_to_cell_index.resize(id + 1,
                                   std::numeric_limits<uint32_t>::max());
      }
      df.id_to_cell_index[id] = static_cast<uint32_t>(df.cells.size());
    }

    for (uint32_t i = 1; i < argc; i += 2) {
      switch (sqlite::value::Type(argv[i])) {
        case sqlite::Type::kText:
          df.cells.emplace_back(sqlite::value::Text(argv[i]));
          break;
        case sqlite::Type::kInteger:
          df.cells.emplace_back(sqlite::value::Int64(argv[i]));
          break;
        case sqlite::Type::kFloat:
          df.cells.emplace_back(sqlite::value::Double(argv[i]));
          break;
        case sqlite::Type::kNull:
          df.cells.emplace_back(std::monostate());
          break;
        case sqlite::Type::kBlob:
          return sqlite::result::Error(
              ctx, "ROW_DATAFRAME_AGG: blob fields not supported");
      }
    }
  }

  static void Final(sqlite3_context* ctx) {
    auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
    if (!raw_agg_ctx) {
      return sqlite::result::Null(ctx);
    }
    return sqlite::result::UniquePointer(
        ctx,
        std::make_unique<perfetto_sql::RowDataframe>(
            std::move(raw_agg_ctx.get()->dataframe)),
        "ROW_DATAFRAME");
  }
};

}  // namespace

base::Status RegisterTypeBuilderFunctions(PerfettoSqlEngine& engine) {
  RETURN_IF_ERROR(engine.RegisterSqliteAggregateFunction<ArrayAgg>(nullptr));
  RETURN_IF_ERROR(engine.RegisterSqliteFunction<Struct>(nullptr));
  RETURN_IF_ERROR(
      engine.RegisterSqliteAggregateFunction<RowDataframeAgg>(nullptr));
  return engine.RegisterSqliteAggregateFunction<NodeAgg>(nullptr);
}

}  // namespace perfetto::trace_processor
