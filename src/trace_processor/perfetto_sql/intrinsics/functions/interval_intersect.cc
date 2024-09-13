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
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/runtime_table.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
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

static const uint32_t kArgCols = 2;
static const uint32_t kIdCols = 5;
static const uint32_t kPartitionColsOffset = kArgCols + kIdCols;

using Intervals = std::vector<Interval>;
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

// Translates partitions to RuntimeTable::Builder types.
base::StatusOr<std::vector<BuilderColType>> GetPartitionsSqlType(
    const Partitions& partitions) {
  auto partition_it = partitions.GetIterator();
  if (!partition_it) {
    return std::vector<BuilderColType>();
  }
  uint32_t p_count =
      static_cast<uint32_t>(partition_it.value().sql_values.size());

  std::vector<BuilderColType> types(p_count, BuilderColType::kNull);
  bool any_part_not_found = true;
  // We expect this loop to be broken very early, but it has to be implemented
  // as loop as we can't deduce the type partition with NULL value.
  for (; partition_it; ++partition_it) {
    any_part_not_found = false;
    for (uint32_t i = 0; i < p_count && any_part_not_found; i++) {
      auto type = types[i];
      if (type != BuilderColType::kNull) {
        continue;
      }
      if (partition_it.value().sql_values[i].is_null()) {
        any_part_not_found = true;
        continue;
      }
      types[i] = FromSqlValueTypeToBuilderType(
          partition_it.value().sql_values[i].type);
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
    const std::vector<Partition*>& partitions) {
  size_t tables_count = partitions.size();

  // Sort `tables_order` from the smallest to the biggest.
  std::vector<uint32_t> tables_order(tables_count);
  std::iota(tables_order.begin(), tables_order.end(), 0);
  std::sort(tables_order.begin(), tables_order.end(),
            [partitions](const uint32_t idx_a, const uint32_t idx_b) {
              return partitions[idx_a]->intervals.size() <
                     partitions[idx_b]->intervals.size();
            });
  uint32_t idx_of_smallest_part = tables_order.front();
  PERFETTO_DCHECK(!partitions[idx_of_smallest_part]->intervals.empty());

  // Trivially translate intervals from smallest table to `MultiIndexIntervals`.
  std::vector<MultiIndexInterval> last_results;
  last_results.reserve(partitions.back()->intervals.size());
  for (const auto& interval : partitions[idx_of_smallest_part]->intervals) {
    MultiIndexInterval m_int;
    m_int.start = interval.start;
    m_int.end = interval.end;
    m_int.idx_in_table.resize(tables_count);
    m_int.idx_in_table[idx_of_smallest_part] = interval.id;
    last_results.push_back(m_int);
  }

  // Create an interval tree on all tables except the smallest - the first one.
  std::vector<MultiIndexInterval> overlaps_with_this_table;
  overlaps_with_this_table.reserve(partitions.back()->intervals.size());
  for (uint32_t i = 1; i < tables_count && !last_results.empty(); i++) {
    overlaps_with_this_table.clear();
    uint32_t table_idx = tables_order[i];

    IntervalIntersector::Mode intersection_mode =
        IntervalIntersector::DecideMode(
            partitions[table_idx]->is_nonoverlapping,
            static_cast<uint32_t>(last_results.size()));
    IntervalIntersector cur_int_operator(partitions[table_idx]->intervals,
                                         intersection_mode);
    for (const auto& prev_result : last_results) {
      Intervals new_overlaps;
      cur_int_operator.FindOverlaps(prev_result.start, prev_result.end,
                                    new_overlaps);
      for (const auto& overlap : new_overlaps) {
        MultiIndexInterval m_int;
        m_int.idx_in_table = std::move(prev_result.idx_in_table);
        m_int.idx_in_table[table_idx] = overlap.id;
        m_int.start = overlap.start;
        m_int.end = overlap.end;
        overlaps_with_this_table.push_back(std::move(m_int));
      }
    }

    last_results = std::move(overlaps_with_this_table);
  }

  uint32_t rows_count = static_cast<uint32_t>(last_results.size());
  std::vector<int64_t> timestamps(rows_count);
  std::vector<int64_t> durations(rows_count);
  std::vector<std::vector<int64_t>> ids(tables_count);
  for (auto& t_ids_vec : ids) {
    t_ids_vec.resize(rows_count);
  }

  for (uint32_t i = 0; i < rows_count; i++) {
    const MultiIndexInterval& interval = last_results[i];
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

  for (uint32_t i = 0; i < partitions[0]->sql_values.size(); i++) {
    const SqlValue& part_val = partitions[0]->sql_values[i];
    switch (part_val.type) {
      case SqlValue::kLong:
        RETURN_IF_ERROR(builder.AddIntegers(i + kPartitionColsOffset,
                                            part_val.AsLong(), rows_count));
        continue;
      case SqlValue::kDouble:
        RETURN_IF_ERROR(builder.AddFloats(i + kPartitionColsOffset,
                                          part_val.AsDouble(), rows_count));
        continue;
      case SqlValue::kString:
        RETURN_IF_ERROR(builder.AddTexts(i + kPartitionColsOffset,
                                         part_val.AsString(), rows_count));
        continue;
      case SqlValue::kNull:
        RETURN_IF_ERROR(builder.AddNulls(i + kPartitionColsOffset, rows_count));
        continue;
      case SqlValue::kBytes:
        PERFETTO_FATAL("Invalid partition type");
    }
  }

  return static_cast<uint32_t>(last_results.size());
}

struct IntervalIntersect : public SqliteFunction<IntervalIntersect> {
  static constexpr char kName[] = "__intrinsic_interval_intersect";
  // Two tables that are being intersected.
  // TODO(mayzner): Support more tables.
  static constexpr int kArgCount = -1;

  struct UserDataContext {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc >= 2);
    size_t tabc = static_cast<size_t>(argc - 1);
    if (tabc > kIdCols) {
      return sqlite::result::Error(
          ctx, "interval intersect: Can intersect at most 5 tables");
    }
    const char* partition_list = sqlite::value::Text(argv[argc - 1]);
    if (!partition_list) {
      return sqlite::result::Error(
          ctx, "interval intersect: column list cannot be null");
    }

    // Get column names of return columns.
    std::vector<std::string> ret_col_names{"ts", "dur"};
    for (uint32_t i = 0; i < kIdCols; i++) {
      ret_col_names.push_back(base::StackString<32>("id_%u", i).ToStdString());
    }
    std::vector<std::string> partition_columns =
        base::SplitString(base::StripChars(partition_list, "()", ' '), ",");
    if (partition_columns.size() > 4) {
      return sqlite::result::Error(
          ctx, "interval intersect: Can take at most 4 partitions.");
    }
    for (const auto& c : partition_columns) {
      std::string p_col_name = base::TrimWhitespace(c).c_str();
      if (!p_col_name.empty()) {
        ret_col_names.push_back(p_col_name);
      }
    }

    // Get data from of each table.
    std::vector<PartitionedTable*> tables(tabc);
    std::vector<Partitions*> t_partitions(tabc);

    for (uint32_t i = 0; i < tabc; i++) {
      tables[i] = sqlite::value::Pointer<PartitionedTable>(
          argv[i], PartitionedTable::kName);

      // If any of the tables is empty the intersection with it also has to be
      // empty.
      if (!tables[i] || tables[i]->partitions_map.size() == 0) {
        SQLITE_ASSIGN_OR_RETURN(
            ctx, std::unique_ptr<RuntimeTable> ret_table,
            RuntimeTable::Builder(GetUserData(ctx)->pool, ret_col_names)
                .Build(0));
        return sqlite::result::UniquePointer(ctx, std::move(ret_table),
                                             "TABLE");
      }
      t_partitions[i] = &tables[i]->partitions_map;
    }

    std::vector<BuilderColType> col_types(kArgCols + tabc,
                                          BuilderColType::kInt);
    // Add dummy id cols.
    col_types.resize(kArgCols + kIdCols, BuilderColType::kNullInt);

    Partitions* p_values = &tables[0]->partitions_map;
    SQLITE_ASSIGN_OR_RETURN(ctx, std::vector<BuilderColType> p_types,
                            GetPartitionsSqlType(*p_values));
    col_types.insert(col_types.end(), p_types.begin(), p_types.end());

    RuntimeTable::Builder builder(GetUserData(ctx)->pool, ret_col_names,
                                  col_types);

    // Partitions will be taken from the table which has the least number of
    // them.
    auto min_el = std::min_element(t_partitions.begin(), t_partitions.end(),
                                   [](const auto& t_a, const auto& t_b) {
                                     return t_a->size() < t_b->size();
                                   });

    auto t_least_partitions =
        static_cast<uint32_t>(std::distance(t_partitions.begin(), min_el));

    // The only partitions we should look at are partitions from the table
    // with the least partitions.
    const Partitions* p_intervals = t_partitions[t_least_partitions];

    // For each partition insert into table.
    uint32_t rows = 0;
    for (auto p_it = p_intervals->GetIterator(); p_it; ++p_it) {
      std::vector<Partition*> cur_partition_in_table;
      bool all_have_p = true;

      // From each table get all vectors of intervals.
      for (uint32_t i = 0; i < tabc; i++) {
        Partitions* t = t_partitions[i];
        if (auto found = t->Find(p_it.key())) {
          cur_partition_in_table.push_back(found);
        } else {
          all_have_p = false;
          break;
        }
      }

      // Only push into the table if all tables have this partition present.
      if (all_have_p) {
        SQLITE_ASSIGN_OR_RETURN(ctx, uint32_t pushed_rows,
                                PushPartition(builder, cur_partition_in_table));
        rows += pushed_rows;
      }
    }

    // Fill the dummy id columns with nulls.
    for (uint32_t i = static_cast<uint32_t>(tabc); i < kIdCols; i++) {
      SQLITE_RETURN_IF_ERROR(ctx, builder.AddNulls(i + kArgCols, rows));
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
