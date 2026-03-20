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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/art_heap_graph_functions.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/json_serializer.h"

namespace perfetto::trace_processor {
namespace {

struct HeapGraphGetArray : public sqlite::Function<HeapGraphGetArray> {
  static constexpr char kName[] = "__intrinsic_heap_graph_get_array";
  static constexpr int kArgCount = 1;
  using UserData = TraceStorage;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == 1);

    if (sqlite::value::Type(argv[0]) == sqlite::Type::kNull) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }

    TraceStorage* storage = GetUserData(ctx);
    auto blob_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
    const auto& blobs = storage->hprof_array_blobs();

    if (blob_id >= blobs.size()) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }

    const auto& blob = blobs[blob_id];
    sqlite::result::StaticBytes(ctx, blob.data(),
                                static_cast<int>(blob.size()));
  }
};

struct HeapGraphGetArrayJson : public sqlite::Function<HeapGraphGetArrayJson> {
  static constexpr char kName[] = "__intrinsic_heap_graph_get_array_json";
  static constexpr int kArgCount = 3;
  using UserData = TraceStorage;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == 3);

    // Return NULL if any argument is NULL.
    for (int i = 0; i < 3; ++i) {
      if (sqlite::value::Type(argv[i]) == sqlite::Type::kNull) {
        return sqlite::utils::ReturnNullFromFunction(ctx);
      }
    }

    TraceStorage* storage = GetUserData(ctx);
    auto blob_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
    std::string_view elem_type(sqlite::value::Text(argv[1]));
    auto count = static_cast<uint32_t>(sqlite::value::Int64(argv[2]));

    const auto& blobs = storage->hprof_array_blobs();
    if (blob_id >= blobs.size()) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }
    const auto& blob = blobs[blob_id];

    // Determine element size and validate blob length.
    size_t elem_size = 0;
    if (elem_type == "boolean" || elem_type == "byte") {
      elem_size = 1;
    } else if (elem_type == "char" || elem_type == "short") {
      elem_size = 2;
    } else if (elem_type == "int" || elem_type == "float") {
      elem_size = 4;
    } else if (elem_type == "long" || elem_type == "double") {
      elem_size = 8;
    } else {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }

    if (static_cast<size_t>(count) * elem_size > blob.size()) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }

    json::JsonSerializer s;
    s.OpenArray();

    const uint8_t* data = blob.data();
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = data + i * elem_size;
      if (elem_type == "boolean") {
        s.BoolValue(p[0] != 0);
      } else if (elem_type == "byte") {
        int8_t v;
        memcpy(&v, p, sizeof(v));
        s.NumberValue(v);
      } else if (elem_type == "short") {
        int16_t v;
        memcpy(&v, p, sizeof(v));
        s.NumberValue(v);
      } else if (elem_type == "int") {
        int32_t v;
        memcpy(&v, p, sizeof(v));
        s.NumberValue(v);
      } else if (elem_type == "long") {
        int64_t v;
        memcpy(&v, p, sizeof(v));
        s.StringValue(std::to_string(v));
      } else if (elem_type == "float") {
        float v;
        memcpy(&v, p, sizeof(v));
        s.FloatValue(v);
      } else if (elem_type == "double") {
        double v;
        memcpy(&v, p, sizeof(v));
        s.DoubleValue(v);
      } else if (elem_type == "char") {
        uint16_t v;
        memcpy(&v, p, sizeof(v));
        s.NumberValue(v);
      }
    }

    s.CloseArray();
    std::string json = s.ToString();
    sqlite::result::TransientString(ctx, json.c_str(),
                                    static_cast<int>(json.size()));
  }
};

}  // namespace

base::Status RegisterArtHeapGraphFunctions(PerfettoSqlEngine* engine,
                                           TraceProcessorContext* context) {
  RETURN_IF_ERROR(
      engine->RegisterFunction<HeapGraphGetArray>(context->storage.get()));
  return engine->RegisterFunction<HeapGraphGetArrayJson>(
      context->storage.get());
}

}  // namespace perfetto::trace_processor
