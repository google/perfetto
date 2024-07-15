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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/interval_intersect.h"

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
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/runtime_table.h"
#include "src/trace_processor/perfetto_sql/engine/function_util.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/partitioned_intervals.h"
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

static const uint32_t kArgCols = 2;
using Intervals = std::vector<IntervalTree::Interval>;
using BuilderColType = RuntimeTable::BuilderColumnType;

struct MultiIndexInterval {
  uint64_t start;
  uint64_t end;
  std::vector<int64_t> idx_in_table;
};

BuilderColType FromSqlValueTypeToBuilderType(SqlValue::Type type) {
  switch (type) {
    case SqlValue::kLong:
      return RuntimeTable::kNullInt;
    case SqlValue::kDouble:
      return RuntimeTable::kNullDouble;
    case SqlValue::kString:
      return RuntimeTable::kString;
    case SqlValue::kNull:
    case SqlValue::kBytes:
      PERFETTO_FATAL("Wrong type");
  }
  PERFETTO_FATAL("For gcc");
}

base::StatusOr<std::vector<BuilderColType>> GetPartitionsSqlType(
    const PartitionToValuesMap& map) {
  auto it = map.GetIterator();
  if (!it) {
    return std::vector<BuilderColType>();
  }
  uint32_t part_count = static_cast<uint32_t>(it.value().size());
  std::vector<BuilderColType> types(part_count, BuilderColType::kNull);
  bool any_part_not_found = true;
  for (; it; ++it) {
    any_part_not_found = false;
    for (uint32_t i = 0; i < part_count && any_part_not_found; i++) {
      auto type = types[i];
      if (type != BuilderColType::kNull) {
        continue;
      }
      if (it.value()[i].is_null()) {
        any_part_not_found = true;
        continue;
      }
      types[i] = FromSqlValueTypeToBuilderType(it.value()[i].type);
    }
  }
  if (any_part_not_found) {
    return base::ErrStatus(
        "INTERVAL_INTERSECT: Can't partition on column that only has NULLs");
  }
  return types;
}

static base::StatusOr<uint32_t> PushPartition(
    RuntimeTable::Builder& builder,
    const std::vector<Intervals*>& table_intervals,
    const std::vector<SqlValue>& partition_values) {
  size_t tables_count = table_intervals.size();
  std::vector<uint32_t> tables_order(tables_count);
  std::iota(tables_order.begin(), tables_order.end(), 0);

  // Sort `tables_order` from the smallest to the biggest
  std::sort(tables_order.begin(), tables_order.end(),
            [table_intervals](const uint32_t idx_a, const uint32_t idx_b) {
              return table_intervals[idx_a]->size() <
                     table_intervals[idx_b]->size();
            });
  uint32_t smallest_table_idx = tables_order.front();
  PERFETTO_DCHECK(!table_intervals[smallest_table_idx]->empty());

  // Trivially translate intervals from smallest table to `MultiIndexIntervals`.
  std::vector<MultiIndexInterval> res;
  res.reserve(table_intervals.back()->size());
  for (const auto& interval : *table_intervals[smallest_table_idx]) {
    MultiIndexInterval m_int;
    m_int.start = interval.start;
    m_int.end = interval.end;
    m_int.idx_in_table.resize(tables_count);
    m_int.idx_in_table[smallest_table_idx] = interval.id;
    res.push_back(m_int);
  }

  // Create an interval tree on all tables except the smallest - the first one.
  std::vector<MultiIndexInterval> overlaps_with_this_table;
  overlaps_with_this_table.reserve(table_intervals.back()->size());
  for (uint32_t i = 1; i < tables_count && !res.empty(); i++) {
    overlaps_with_this_table.clear();
    uint32_t table_idx = tables_order[i];
    IntervalTree cur_tree(*table_intervals[table_idx]);
    for (const auto& r : res) {
      Intervals new_intervals;
      cur_tree.FindOverlaps(r.start, r.end, new_intervals);
      for (const auto& overlap : new_intervals) {
        MultiIndexInterval m_int;
        m_int.idx_in_table = std::move(r.idx_in_table);
        m_int.idx_in_table[table_idx] = overlap.id;
        m_int.start = overlap.start;
        m_int.end = overlap.end;
        overlaps_with_this_table.push_back(std::move(m_int));
      }
    }

    res = std::move(overlaps_with_this_table);
  }

  uint32_t rows_count = static_cast<uint32_t>(res.size());
  std::vector<int64_t> timestamps(rows_count);
  std::vector<int64_t> durations(rows_count);
  std::vector<std::vector<int64_t>> ids(tables_count);
  for (auto& t_ids_vec : ids) {
    t_ids_vec.resize(rows_count);
  }

  for (uint32_t i = 0; i < rows_count; i++) {
    const MultiIndexInterval& interval = res[i];
    timestamps[i] = static_cast<int64_t>(interval.start);
    durations[i] = static_cast<int64_t>(interval.end) -
                   static_cast<int64_t>(interval.start);
    for (uint32_t j = 0; j < tables_count; j++) {
      ids[j][i] = interval.idx_in_table[j];
    }
  }

  builder.AddNonNullIntegersUnchecked(0, std::move(timestamps));
  builder.AddNonNullIntegersUnchecked(1, std::move(durations));
  for (uint32_t i = 0; i < tables_count; i++) {
    builder.AddNonNullIntegersUnchecked(i + kArgCols, ids[i]);
  }

  uint32_t res_size = static_cast<uint32_t>(res.size());
  for (uint32_t i = 0; i < partition_values.size(); i++) {
    const SqlValue& part_val = partition_values[i];
    switch (part_val.type) {
      case SqlValue::kLong:
        RETURN_IF_ERROR(builder.AddIntegers(
            i + kArgCols + static_cast<uint32_t>(tables_count),
            part_val.AsLong(), res_size));
        continue;
      case SqlValue::kDouble:
        RETURN_IF_ERROR(builder.AddFloats(
            i + kArgCols + static_cast<uint32_t>(tables_count),
            part_val.AsDouble(), res_size));
        continue;
      case SqlValue::kString:
        RETURN_IF_ERROR(
            builder.AddTexts(i + kArgCols + static_cast<uint32_t>(tables_count),
                             part_val.AsString(), res_size));
        continue;
      case SqlValue::kNull:
        RETURN_IF_ERROR(builder.AddNulls(
            i + kArgCols + static_cast<uint32_t>(tables_count), res_size));
        continue;
      case SqlValue::kBytes:
        PERFETTO_FATAL("Invalid partition type");
    }
  }

  return static_cast<uint32_t>(res.size());
}

struct IntervalIntersect : public SqliteFunction<IntervalIntersect> {
  static constexpr char kName[] = "__intrinsic_interval_intersect";
  // Two tables that are being intersected.
  // TODO(mayzner): Support more tables.
  static constexpr int kArgCount = 3;

  struct UserDataContext {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc >= 2);
    size_t tabc = static_cast<size_t>(argc - 1);
    const char* partition_list = sqlite::value::Text(argv[argc - 1]);
    if (!partition_list) {
      return sqlite::result::Error(
          ctx, "interval intersect: column list cannot be null");
    }

    // Get column names of return columns.
    std::vector<std::string> ret_col_names{"ts", "dur"};
    for (uint32_t i = 0; i < tabc; i++) {
      ret_col_names.push_back(base::StackString<32>("id_%u", i).ToStdString());
    }

    for (const auto& c :
         base::SplitString(base::StripChars(partition_list, "()", ' '), ",")) {
      std::string p_col_name = base::TrimWhitespace(c).c_str();
      if (!p_col_name.empty()) {
        ret_col_names.push_back(base::TrimWhitespace(c));
      }
    }

    // Get data from of each table.
    std::vector<PartitionedTable*> tables(tabc);
    std::vector<PartitionToIntervalsMap*> t_intervals(tabc);

    for (uint32_t i = 0; i < tabc; i++) {
      tables[i] = sqlite::value::Pointer<PartitionedTable>(
          argv[i], PartitionedTable::kName);

      // If any of the tables is empty the intersection with it also has to be
      // empty.
      if (!tables[i] || tables[i]->intervals.size() == 0) {
        SQLITE_ASSIGN_OR_RETURN(
            ctx, std::unique_ptr<RuntimeTable> ret_table,
            RuntimeTable::Builder(GetUserData(ctx)->pool, ret_col_names)
                .Build(0));
        return sqlite::result::UniquePointer(ctx, std::move(ret_table),
                                             "TABLE");
      }
      t_intervals[i] = &tables[i]->intervals;
    }

    std::vector<BuilderColType> col_types(kArgCols + tabc,
                                          BuilderColType::kInt);
    PartitionToValuesMap* p_values = &tables[0]->partition_values;
    SQLITE_ASSIGN_OR_RETURN(ctx, std::vector<BuilderColType> p_types,
                            GetPartitionsSqlType(*p_values));
    col_types.insert(col_types.end(), p_types.begin(), p_types.end());

    RuntimeTable::Builder builder(GetUserData(ctx)->pool, ret_col_names,
                                  col_types);

    // Partitions will be taken from the table which has the least number of
    // them.
    auto min_el = std::min_element(t_intervals.begin(), t_intervals.end(),
                                   [](const auto& t_a, const auto& t_b) {
                                     return t_a->size() < t_b->size();
                                   });

    auto t_least_partitions =
        static_cast<uint32_t>(std::distance(t_intervals.begin(), min_el));

    // The only partitions we should look at are partitions from the table
    // with the least partitions.
    const PartitionToIntervalsMap* p_intervals =
        t_intervals[t_least_partitions];

    // For each partition insert into table.
    uint32_t rows = 0;
    for (auto p_it = p_intervals->GetIterator(); p_it; ++p_it) {
      std::vector<Intervals*> unpartitioned_intervals;
      bool all_have_p = true;

      // From each table get all vectors of intervals.
      for (uint32_t i = 0; i < tabc; i++) {
        PartitionToIntervalsMap* t = t_intervals[i];
        if (auto found = t->Find(p_it.key())) {
          unpartitioned_intervals.push_back(found);
        } else {
          all_have_p = false;
          break;
        }
      }

      // Only push into the table if all tables have this partition present.
      if (all_have_p) {
        SQLITE_ASSIGN_OR_RETURN(ctx, uint32_t pushed_rows,
                                PushPartition(builder, unpartitioned_intervals,
                                              (*p_values)[p_it.key()]));
        rows += pushed_rows;
      }
    }

    SQLITE_ASSIGN_OR_RETURN(ctx, std::unique_ptr<RuntimeTable> ret_tab,
                            std::move(builder).Build(rows));

    return sqlite::result::UniquePointer(ctx, std::move(ret_tab), "TABLE");
  }
};

}  // namespace

base::Status RegisterIntervalIntersectFunctions(PerfettoSqlEngine& engine,
                                                StringPool* pool) {
  return engine.RegisterSqliteFunction<IntervalIntersect>(
      std::make_unique<IntervalIntersect::UserDataContext>(
          IntervalIntersect::UserDataContext{&engine, pool}));
}

}  // namespace perfetto::trace_processor::perfetto_sql
