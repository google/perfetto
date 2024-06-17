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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/struct.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <variant>

#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/struct.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

// An SQL scalar function which creates an struct.
// TODO(lalitm): once we have some stability here, expand the comments
// here.
struct Struct : public SqliteFunction<Struct> {
  static constexpr char kName[] = "__intrinsic_struct";
  static constexpr int kArgCount = -1;

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
};

void Struct::Step(sqlite3_context* ctx, int rargc, sqlite3_value** argv) {
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
      return sqlite::result::Error(ctx, "STRUCT: field names must be strings");
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
        return sqlite::result::Error(ctx, "STRUCT: blob fields not supported");
    }
  }
  sqlite::result::RawPointer(ctx, s.release(), "STRUCT", [](void* ptr) {
    std::unique_ptr<perfetto_sql::Struct>(
        static_cast<perfetto_sql::Struct*>(ptr));
  });
}

}  // namespace

base::Status RegisterStructFunctions(PerfettoSqlEngine& engine) {
  return engine.RegisterSqliteFunction<Struct>(nullptr);
}

}  // namespace perfetto::trace_processor
