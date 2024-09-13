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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/counter_intervals.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/runtime_table.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/counter.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/partitioned_intervals.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/sqlite/bindings/sqlite_bind.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_stmt.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::perfetto_sql {
namespace {

struct CounterIntervals : public SqliteFunction<CounterIntervals> {
  static constexpr char kName[] = "__intrinsic_counter_intervals";
  static constexpr int kArgCount = 3;

  struct UserDataContext {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);
    const char* leading_str = sqlite::value::Text(argv[0]);
    if (!leading_str) {
      return sqlite::result::Error(
          ctx, "interval intersect: column list cannot be null");
    }

    // TODO(mayzner): Support 'lagging'.
    if (base::CaseInsensitiveEqual("lagging", leading_str)) {
      return sqlite::result::Error(
          ctx, "interval intersect: 'lagging' is not implemented");
    }
    if (!base::CaseInsensitiveEqual("leading", leading_str)) {
      return sqlite::result::Error(ctx,
                                   "interval intersect: second argument has to "
                                   "be either 'leading' or 'lagging");
    }

    int64_t trace_end = sqlite::value::Int64(argv[1]);

    // Get column names of return columns.
    std::vector<std::string> ret_col_names{
        "id", "ts", "dur", "track_id", "value", "next_value", "delta_value"};
    std::vector<RuntimeTable::BuilderColumnType> col_types{
        RuntimeTable::kInt,         // id
        RuntimeTable::kInt,         // ts,
        RuntimeTable::kInt,         // dur
        RuntimeTable::kInt,         // track_id
        RuntimeTable::kDouble,      // value
        RuntimeTable::kNullDouble,  // next_value
        RuntimeTable::kNullDouble,  // delta_value
    };

    auto partitioned_counter = sqlite::value::Pointer<PartitionedCounter>(
        argv[2], PartitionedCounter::kName);
    if (!partitioned_counter) {
      SQLITE_ASSIGN_OR_RETURN(
          ctx, std::unique_ptr<RuntimeTable> ret_table,
          RuntimeTable::Builder(GetUserData(ctx)->pool, ret_col_names)
              .Build(0));
      return sqlite::result::UniquePointer(ctx, std::move(ret_table), "TABLE");
    }

    RuntimeTable::Builder builder(GetUserData(ctx)->pool, ret_col_names,
                                  col_types);

    uint32_t rows_count = 0;
    for (auto track_counter = partitioned_counter->partitions_map.GetIterator();
         track_counter; ++track_counter) {
      int64_t track_id = track_counter.key();
      const auto& cols = track_counter.value();
      size_t r_count = cols.id.size();
      rows_count += r_count;

      // Id
      builder.AddNonNullIntegersUnchecked(0, cols.id);
      // Ts
      builder.AddNonNullIntegersUnchecked(1, cols.ts);

      // Dur
      std::vector<int64_t> dur(r_count);
      for (size_t i = 0; i < r_count - 1; i++) {
        dur[i] = cols.ts[i + 1] - cols.ts[i];
      }
      dur[r_count - 1] = trace_end - cols.ts.back();
      builder.AddNonNullIntegersUnchecked(2, dur);

      // Track id
      builder.AddIntegers(3, track_id, static_cast<uint32_t>(r_count));
      // Value
      builder.AddNonNullDoublesUnchecked(4, cols.val);

      // Next value
      std::vector<double> next_vals(cols.val.begin() + 1, cols.val.end());
      builder.AddNullDoublesUnchecked(5, next_vals);
      builder.AddNull(5);

      // Delta value
      std::vector<double> deltas(r_count - 1);
      for (size_t i = 0; i < r_count - 1; i++) {
        deltas[i] = cols.val[i + 1] - cols.val[i];
      }
      builder.AddNull(6);
      builder.AddNullDoublesUnchecked(6, deltas);
    }

    SQLITE_ASSIGN_OR_RETURN(ctx, std::unique_ptr<RuntimeTable> ret_tab,
                            std::move(builder).Build(rows_count));

    return sqlite::result::UniquePointer(ctx, std::move(ret_tab), "TABLE");
  }
};

}  // namespace

base::Status RegisterCounterIntervalsFunctions(PerfettoSqlEngine& engine,
                                               StringPool* pool) {
  return engine.RegisterSqliteFunction<CounterIntervals>(
      std::make_unique<CounterIntervals::UserDataContext>(
          CounterIntervals::UserDataContext{&engine, pool}));
}

}  // namespace perfetto::trace_processor::perfetto_sql
