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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/array.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {
namespace {

using ArrayVariant = std::variant<std::vector<int64_t>,
                                  std::vector<double>,
                                  std::vector<std::string>>;

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
    return sqlite::result::RawPointer(ctx, res.release(), type, [](void* ptr) {
      std::unique_ptr<std::vector<T>>(static_cast<std::vector<T>*>(ptr));
    });
  }

  std::optional<ArrayVariant> array;
};

// An SQL aggregate-function which creates an array.
struct ArrayAgg : public SqliteAggregateFunction<ArrayAgg> {
  static constexpr char kName[] = "__intrinsic_array_agg";
  static constexpr int kArgCount = 1;

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
  static void Final(sqlite3_context* ctx);
};

void ArrayAgg::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
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
      return sqlite::result::Error(ctx, "ARRAY_AGG: blobs are not supported.");
  }
}

void ArrayAgg::Final(sqlite3_context* ctx) {
  auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
  if (!raw_agg_ctx) {
    return sqlite::result::Null(ctx);
  }

  auto& array = *raw_agg_ctx.get()->array;
  switch (array.index()) {
    case 0 /* int64_t */:
      return raw_agg_ctx.get()->Result<int64_t>(ctx, "ARRAY<INT64>");
    case 1 /* double */:
      return raw_agg_ctx.get()->Result<double>(ctx, "ARRAY<DOUBLE>");
    case 2 /* std::string */:
      return raw_agg_ctx.get()->Result<std::string>(ctx, "ARRAY<STRING>");
  }
}

}  // namespace

base::Status RegisterArrayFunctions(PerfettoSqlEngine& engine) {
  return engine.RegisterSqliteAggregateFunction<ArrayAgg>(nullptr);
}

}  // namespace perfetto::trace_processor
