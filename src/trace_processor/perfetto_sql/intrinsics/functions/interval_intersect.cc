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

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
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

namespace perfetto::trace_processor {
namespace {

static const uint32_t kArgCols = 2;

struct MultiIndexInterval {
  uint64_t start;
  uint64_t end;
  std::vector<uint32_t> idx_in_table;

  base::Status AddRow(RuntimeTable::Builder& builder,
                      uint32_t table_count) const {
    builder.AddNonNullIntegerUnchecked(0, static_cast<int64_t>(start));
    builder.AddNonNullIntegerUnchecked(1, static_cast<int64_t>(end - start));
    for (uint32_t i = 0; i < table_count; i++) {
      builder.AddNonNullIntegerUnchecked(i + kArgCols,
                                         static_cast<int64_t>(idx_in_table[i]));
    }
    return base::OkStatus();
  }
};

static base::StatusOr<uint32_t> PushUnpartitioned(
    RuntimeTable::Builder& builder,
    const std::vector<std::vector<IntervalTree::Interval>*>& table_intervals) {
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

  // Create an interval tree on all tables except the smallest - the first one
  std::vector<MultiIndexInterval> overlaps_with_this_table;
  overlaps_with_this_table.reserve(table_intervals.back()->size());
  for (uint32_t i = 1; i < tables_count && !res.empty(); i++) {
    overlaps_with_this_table.clear();
    uint32_t table_idx = tables_order[i];
    IntervalTree cur_tree(*table_intervals[table_idx]);
    for (const auto& r : res) {
      std::vector<IntervalTree::Interval> new_intervals;
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

  for (const MultiIndexInterval& interval : res) {
    RETURN_IF_ERROR(
        interval.AddRow(builder, static_cast<uint32_t>(tables_count)));
  }

  return static_cast<uint32_t>(res.size());
}

struct IntervalIntersect : public SqliteFunction<IntervalIntersect> {
  static constexpr char kName[] = "__intrinsic_interval_intersect";
  // Two tables that are being intersected.
  // TODO(mayzner): Support more tables.
  static constexpr int kArgCount = 2;

  struct UserDataContext {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc >= 2);
    size_t t_count = static_cast<size_t>(argc);

    // Get returned table Builder.
    std::vector<std::string> ret_col_names;
    ret_col_names.push_back("ts");
    ret_col_names.push_back("dur");
    for (uint32_t i = 0; i < t_count; i++) {
      base::StackString<32> x("id_%u", i);
      ret_col_names.push_back(x.ToStdString());
    }
    std::vector<RuntimeTable::BuilderColumnType> column_types(
        ret_col_names.size(), RuntimeTable::BuilderColumnType::kInt);
    RuntimeTable::Builder builder(GetUserData(ctx)->pool, ret_col_names,
                                  std::move(column_types));

    // Fetch data from tables.
    std::vector<perfetto_sql::PartitionedIntervals*> tables(t_count);
    for (uint32_t i = 0; i < t_count; i++) {
      tables[i] = sqlite::value::Pointer<perfetto_sql::PartitionedIntervals>(
          argv[i], "INTERVAL_TREE_PARTITIONS");

      // If any of the tables is empty the intersection with it also has to be
      // empty.
      if (!tables[i] || (tables[i]->size() == 0)) {
        SQLITE_ASSIGN_OR_RETURN(ctx, std::unique_ptr<RuntimeTable> ret_table,
                                std::move(builder).Build(0));
        return sqlite::result::UniquePointer(ctx, std::move(ret_table),
                                             "TABLE");
      }
    }

    // Partitions will be taken from the table which has the least number of
    // them.
    auto min_el = std::min_element(tables.begin(), tables.end(),
                                   [tables](const auto& t_a, const auto& t_b) {
                                     return t_a->size() < t_b->size();
                                   });
    auto t_least_partitions =
        static_cast<uint32_t>(std::distance(tables.begin(), min_el));

    // The only partitions we should look at are partitions from the table with
    // the least partitions.
    const auto& p_intervals = tables[t_least_partitions];

    // For each partition insert into table.
    uint32_t rows = 0;
    for (auto p_it = p_intervals->GetIterator(); p_it; ++p_it) {
      std::vector<std::vector<IntervalTree::Interval>*> unpartitioned_intervals;
      bool all_have_p = true;

      // From each table get all vectors of intervals.
      for (uint32_t i = 0; i < t_count; i++) {
        perfetto_sql::PartitionedIntervals* t = tables[i];
        if (auto found = t->Find(p_it.key())) {
          unpartitioned_intervals.push_back(found);
        } else {
          all_have_p = false;
          break;
        }
      }

      // Only push into the table if all tables have this partition present.
      if (all_have_p) {
        SQLITE_ASSIGN_OR_RETURN(
            ctx, uint32_t pushed_rows,
            PushUnpartitioned(builder, unpartitioned_intervals));
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

}  // namespace perfetto::trace_processor
