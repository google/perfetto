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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/heap_graph_functions.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {

// __INTRINSIC_HEAP_GRAPH_GET_ARRAY(array_data_id INT) -> BLOB
//
// Returns the raw bytes of a primitive array stored during HPROF import.
// The blob is in native little-endian format; element type and count are
// available via the array_element_type and array_element_count columns on
// heap_graph_object.
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
    // StaticBytes: blob lives in TraceStorage for the trace lifetime.
    sqlite::result::StaticBytes(ctx, blob.data(),
                                static_cast<int>(blob.size()));
  }
};

}  // namespace

base::Status RegisterHeapGraphFunctions(PerfettoSqlEngine* engine,
                                        TraceProcessorContext* context) {
  return engine->RegisterFunction<HeapGraphGetArray>(context->storage.get());
}

}  // namespace perfetto::trace_processor
