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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/interval_self_intersect.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/dataframe/dataframe.h"
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

namespace perfetto::trace_processor::perfetto_sql {
namespace {

using ColType = dataframe::AdhocDataframeBuilder::ColumnType;

// Event for sweep line algorithm.
// Events are sorted by timestamp, then by type (starts before ends), then by
// ID (ascending for starts, descending for ends). The descending order for end
// events ensures that when multiple intervals end at the same timestamp, we
// emit an instant capturing the state after the first interval ends.
struct Event {
  int64_t ts;
  uint32_t id;
  bool is_start;

  bool operator<(const Event& other) const {
    if (ts != other.ts)
      return ts < other.ts;
    if (is_start != other.is_start)
      return is_start > other.is_start;
    if (!is_start)
      return id > other.id;
    return id < other.id;
  }
};

// Supported aggregation types for interval intersections.
enum class AggType {
  kCount,
  kSum,
  kMin,
  kMax,
  kAvg,
};

// Specification for a column to aggregate during interval intersection.
struct AggColumn {
  std::string name;
  AggType type;
  uint32_t col_idx;  // Index in the aggregation data array
};

// Aggregation specification passed as a pointer between SQL functions.
struct IntervalAggSpec {
  static constexpr char kPointerType[] = "INTERVAL_AGG";
  std::string column_name;
  AggType agg_type;

  IntervalAggSpec(std::string col, AggType type)
      : column_name(std::move(col)), agg_type(type) {}
};

// Maintains aggregation state for a single intersection bucket.
struct BucketAggregator {
  uint32_t count = 0;
  std::vector<double> sums;
  std::vector<std::optional<double>> mins;
  std::vector<std::optional<double>> maxs;

  void Reset(size_t num_aggs) {
    count = 0;
    // Use std::fill instead of assign to avoid reallocation
    if (sums.size() != num_aggs) {
      sums.resize(num_aggs);
      mins.resize(num_aggs);
      maxs.resize(num_aggs);
    }
    std::fill(sums.begin(), sums.end(), 0.0);
    std::fill(mins.begin(), mins.end(), std::nullopt);
    std::fill(maxs.begin(), maxs.end(), std::nullopt);
  }

  void AddValue(size_t agg_idx, double value) {
    sums[agg_idx] += value;
    if (!mins[agg_idx] || value < *mins[agg_idx]) {
      mins[agg_idx] = value;
    }
    if (!maxs[agg_idx] || value > *maxs[agg_idx]) {
      maxs[agg_idx] = value;
    }
  }
};

// Computes self-intersections for a single partition using a sweep line
// algorithm. Returns a new partition with non-overlapping intervals
// representing the intersection structure.
base::StatusOr<Partition> ComputePartitionIntersection(
    const Partition& partition,
    const std::vector<AggColumn>& agg_columns,
    const std::vector<std::vector<SqlValue>>& interval_data) {
  Partition result;
  result.sql_values = partition.sql_values;
  result.is_nonoverlapping = true;

  if (partition.intervals.empty()) {
    return result;
  }

  // Build events for sweep line algorithm.
  std::vector<Event> events;
  events.reserve(partition.intervals.size() * 2);

  for (const auto& interval : partition.intervals) {
    events.push_back(Event{static_cast<int64_t>(interval.start),
                           static_cast<uint32_t>(interval.id), true});
    events.push_back(Event{static_cast<int64_t>(interval.end),
                           static_cast<uint32_t>(interval.id), false});
  }

  std::sort(events.begin(), events.end());

  // Track which intervals are currently active.
  base::FlatHashMap<uint32_t, bool> active_set;
  std::vector<uint32_t> active_ids;
  active_ids.reserve(partition.intervals.size());
  uint32_t active_count = 0;

  int64_t prev_ts = events[0].ts;
  uint32_t group_id = 0;

  BucketAggregator agg;
  agg.Reset(agg_columns.size());

  auto emit_bucket = [&](int64_t start_ts, int64_t end_ts) {
    if (start_ts > end_ts || active_count == 0)
      return;

    // Compute aggregations for currently active intervals.
    agg.Reset(agg_columns.size());
    agg.count = active_count;

    if (!agg_columns.empty() && !interval_data.empty()) {
      for (uint32_t id : active_ids) {
        if (id < interval_data.size() && !interval_data[id].empty()) {
          for (size_t agg_idx = 0; agg_idx < agg_columns.size(); ++agg_idx) {
            const auto& agg_col = agg_columns[agg_idx];
            if (agg_col.type != AggType::kCount &&
                agg_col.col_idx < interval_data[id].size()) {
              const auto& val = interval_data[id][agg_col.col_idx];
              if (!val.is_null()) {
                double value = val.type == SqlValue::kLong
                                   ? static_cast<double>(val.long_value)
                                   : val.double_value;
                agg.AddValue(agg_idx, value);
              }
            }
          }
        }
      }
    }

    // Store interval and aggregation results.
    result.intervals.push_back(Interval{static_cast<uint64_t>(start_ts),
                                        static_cast<uint64_t>(end_ts),
                                        static_cast<uint32_t>(group_id)});

    std::vector<SqlValue> row_agg_data;
    row_agg_data.reserve(agg_columns.size());

    for (size_t agg_idx = 0; agg_idx < agg_columns.size(); ++agg_idx) {
      const auto& agg_col = agg_columns[agg_idx];
      switch (agg_col.type) {
        case AggType::kCount:
          row_agg_data.push_back(SqlValue::Long(agg.count));
          break;
        case AggType::kSum:
          row_agg_data.push_back(SqlValue::Double(agg.sums[agg_idx]));
          break;
        case AggType::kMin:
          if (agg.mins[agg_idx]) {
            row_agg_data.push_back(SqlValue::Double(*agg.mins[agg_idx]));
          } else {
            row_agg_data.push_back(SqlValue());
          }
          break;
        case AggType::kMax:
          if (agg.maxs[agg_idx]) {
            row_agg_data.push_back(SqlValue::Double(*agg.maxs[agg_idx]));
          } else {
            row_agg_data.push_back(SqlValue());
          }
          break;
        case AggType::kAvg:
          if (agg.count > 0) {
            row_agg_data.push_back(
                SqlValue::Double(agg.sums[agg_idx] / agg.count));
          } else {
            row_agg_data.push_back(SqlValue());
          }
          break;
      }
    }
    result.agg_data.push_back(std::move(row_agg_data));
  };

  // Sweep line algorithm: process events in sorted order.
  bool emitted_instant_at_current_ts = false;
  for (size_t event_idx = 0; event_idx < events.size(); ++event_idx) {
    const auto& event = events[event_idx];

    // Emit bucket for the previous timestamp range when we reach a new
    // timestamp.
    if (event.ts > prev_ts) {
      emit_bucket(prev_ts, event.ts);
      group_id++;
      prev_ts = event.ts;
      emitted_instant_at_current_ts = false;
    }

    // Update the active interval set.
    if (event.is_start) {
      if (!active_set.Find(event.id)) {
        active_set[event.id] = true;
        active_ids.push_back(event.id);
        active_count++;
      }
    } else {
      if (active_set.Find(event.id)) {
        active_set.Erase(event.id);
        auto it = std::find(active_ids.begin(), active_ids.end(), event.id);
        if (it != active_ids.end()) {
          active_ids.erase(it);
        }
        active_count--;
      }

      // After processing the first end event at this timestamp, emit an instant
      // if there are still active intervals and more events at this timestamp.
      // This captures the state between interval endings.
      if (!emitted_instant_at_current_ts && active_count > 0 &&
          event_idx + 1 < events.size() &&
          events[event_idx + 1].ts == event.ts) {
        emit_bucket(event.ts, event.ts);
        group_id++;
        emitted_instant_at_current_ts = true;
      }
    }
  }

  return result;
}

// Helper to extract a typed pointer from a SQLite value with error checking.
template <typename T>
base::StatusOr<T*> GetPointerOrError(sqlite3_value* value,
                                     const char* func_name) {
  T* ptr = sqlite::value::Pointer<T>(value, T::kPointerType);
  if (!ptr) {
    return base::ErrStatus("%s: expected %s pointer", func_name,
                           T::kPointerType);
  }
  return ptr;
}

// __intrinsic_interval_agg: Creates an aggregation specification for use with
// interval_intersect. Returns an opaque pointer to IntervalAggSpec.
struct IntervalAggFn : public sqlite::Function<IntervalAggFn> {
  static constexpr char kName[] = "__intrinsic_interval_agg";
  static constexpr int kArgCount = 2;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 2) {
      return sqlite::result::Error(
          ctx, "interval_agg: Expected 2 arguments (column_name, agg_type)");
    }

    if (sqlite::value::Type(argv[0]) != sqlite::Type::kText ||
        sqlite::value::Type(argv[1]) != sqlite::Type::kText) {
      return sqlite::result::Error(
          ctx, "interval_agg: Both arguments must be strings");
    }

    std::string col_name = sqlite::value::Text(argv[0]);
    std::string agg_type_str = sqlite::value::Text(argv[1]);

    AggType agg_type;
    if (agg_type_str == "COUNT") {
      agg_type = AggType::kCount;
    } else if (agg_type_str == "SUM") {
      agg_type = AggType::kSum;
    } else if (agg_type_str == "MIN") {
      agg_type = AggType::kMin;
    } else if (agg_type_str == "MAX") {
      agg_type = AggType::kMax;
    } else if (agg_type_str == "AVG") {
      agg_type = AggType::kAvg;
    } else {
      return sqlite::result::Error(
          ctx,
          ("interval_agg: Unknown aggregation type: " + agg_type_str).c_str());
    }

    auto spec =
        std::make_unique<IntervalAggSpec>(std::move(col_name), agg_type);
    return sqlite::result::UniquePointer(ctx, std::move(spec),
                                         IntervalAggSpec::kPointerType);
  }
};

// __intrinsic_interval_self_intersect: Computes self-intersections of intervals
// within a partitioned table. Takes a PartitionedTable and optional aggregation
// specifications, returning a new PartitionedTable with non-overlapping
// intervals representing the intersection structure.
struct IntervalSelfIntersect : public sqlite::Function<IntervalSelfIntersect> {
  static constexpr char kName[] = "__intrinsic_interval_self_intersect";
  static constexpr int kArgCount = -1;

  struct UserData {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc < 1) {
      return sqlite::result::Error(ctx,
                                   "interval_self_intersect: Expected at least "
                                   "1 argument (table, [agg_specs...])");
    }

    // Parse aggregation specifications from function arguments.
    std::vector<AggColumn> agg_columns;
    base::FlatHashMap<std::string, uint32_t> col_name_to_idx;

    for (int i = 1; i < argc; ++i) {
      SQLITE_ASSIGN_OR_RETURN(
          ctx, auto* agg_spec,
          GetPointerOrError<IntervalAggSpec>(argv[i], kName));

      // Assign indices to aggregation columns (COUNT doesn't need data).
      if (agg_spec->agg_type != AggType::kCount &&
          !col_name_to_idx.Find(agg_spec->column_name)) {
        uint32_t idx = static_cast<uint32_t>(col_name_to_idx.size());
        col_name_to_idx[agg_spec->column_name] = idx;
      }

      AggColumn agg_col;
      agg_col.name = agg_spec->column_name;
      agg_col.type = agg_spec->agg_type;
      agg_col.col_idx = agg_spec->agg_type == AggType::kCount
                            ? 0
                            : *col_name_to_idx.Find(agg_spec->column_name);
      agg_columns.push_back(std::move(agg_col));
    }

    // Process input table.
    PartitionedTable* table = sqlite::value::Pointer<PartitionedTable>(
        argv[0], PartitionedTable::kName);

    if (!table) {
      return sqlite::result::Error(ctx, "Invalid table pointer");
    }

    auto ret_table = std::make_unique<PartitionedTable>();
    ret_table->partition_column_names = table->partition_column_names;

    // Generate output column names with appropriate prefixes.
    for (const auto& agg : agg_columns) {
      std::string col_name;
      switch (agg.type) {
        case AggType::kCount:
          col_name = "count";
          break;
        case AggType::kSum:
          col_name = "sum_" + agg.name;
          break;
        case AggType::kMin:
          col_name = "min_" + agg.name;
          break;
        case AggType::kMax:
          col_name = "max_" + agg.name;
          break;
        case AggType::kAvg:
          col_name = "avg_" + agg.name;
          break;
      }
      ret_table->agg_column_names.push_back(col_name);
    }

    // Compute intersections for each partition independently.
    for (auto p_it = table->partitions_map.GetIterator(); p_it; ++p_it) {
      const auto& interval_data = p_it.value().agg_data;
      auto status_or_partition = ComputePartitionIntersection(
          p_it.value(), agg_columns, interval_data);
      if (!status_or_partition.ok()) {
        return sqlite::result::Error(ctx,
                                     status_or_partition.status().c_message());
      }
      ret_table->partitions_map.Insert(p_it.key(),
                                       std::move(status_or_partition.value()));
    }

    return sqlite::result::UniquePointer(ctx, std::move(ret_table),
                                         PartitionedTable::kName);
  }
};

// __intrinsic_interval_to_table: Converts a PartitionedTable (containing
// intervals and aggregation data) into a regular SQL table (Dataframe).
struct IntervalsToTable : public sqlite::Function<IntervalsToTable> {
  static constexpr char kName[] = "__intrinsic_interval_to_table";
  static constexpr int kArgCount = 1;

  struct UserData {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 1) {
      return sqlite::result::Error(
          ctx, "interval_to_table: Expected 1 argument (table)");
    }

    // Get input table
    PartitionedTable* table = sqlite::value::Pointer<PartitionedTable>(
        argv[0], PartitionedTable::kName);

    // Get column names for result
    std::vector<std::string> ret_col_names{"ts", "dur", "group_id"};

    if (table) {
      ret_col_names.insert(ret_col_names.end(), table->agg_column_names.begin(),
                           table->agg_column_names.end());
      ret_col_names.insert(ret_col_names.end(),
                           table->partition_column_names.begin(),
                           table->partition_column_names.end());
    }

    // Build column types
    std::vector<ColType> col_types{ColType::kInt64, ColType::kInt64,
                                   ColType::kInt64};  // ts, dur, group_id

    if (!table || table->partitions_map.size() == 0) {
      // Empty table
      dataframe::AdhocDataframeBuilder builder(
          ret_col_names, GetUserData(ctx)->pool, col_types);
      SQLITE_ASSIGN_OR_RETURN(ctx, dataframe::Dataframe ret_table,
                              std::move(builder).Build());
      return sqlite::result::UniquePointer(
          ctx, std::make_unique<dataframe::Dataframe>(std::move(ret_table)),
          "TABLE");
    }

    // Add aggregation column types
    // We assume they are Double except Count which is Int64. But we don't store
    // types. Let's inspect the first partition's agg data
    auto it = table->partitions_map.GetIterator();
    if (it && !it.value().agg_data.empty() && !it.value().agg_data[0].empty()) {
      for (const auto& val : it.value().agg_data[0]) {
        if (val.type == SqlValue::kLong) {
          col_types.push_back(ColType::kInt64);
        } else {
          col_types.push_back(ColType::kDouble);
        }
      }
    } else {
      // Fallback: assume all doubles
      for (size_t i = 0; i < table->agg_column_names.size(); ++i) {
        col_types.push_back(ColType::kDouble);
      }
    }

    // Add partition column types
    if (it) {
      for (const auto& val : it.value().sql_values) {
        if (val.type == SqlValue::kLong) {
          col_types.push_back(ColType::kInt64);
        } else if (val.type == SqlValue::kDouble) {
          col_types.push_back(ColType::kDouble);
        } else if (val.type == SqlValue::kString) {
          col_types.push_back(ColType::kString);
        } else {
          col_types.push_back(ColType::kInt64);  // Default
        }
      }
    }

    dataframe::AdhocDataframeBuilder builder(ret_col_names,
                                             GetUserData(ctx)->pool, col_types);

    for (auto p_it = table->partitions_map.GetIterator(); p_it; ++p_it) {
      const auto& partition = p_it.value();

      // Pre-intern strings for partition columns
      std::vector<StringPool::Id> partition_string_ids;
      partition_string_ids.reserve(partition.sql_values.size());
      for (const auto& part_val : partition.sql_values) {
        if (part_val.type == SqlValue::kString) {
          partition_string_ids.push_back(
              GetUserData(ctx)->pool->InternString(part_val.string_value));
        } else {
          partition_string_ids.push_back(StringPool::Id::Null());
        }
      }

      for (size_t i = 0; i < partition.intervals.size(); ++i) {
        const auto& interval = partition.intervals[i];

        builder.PushNonNullUnchecked(0, static_cast<int64_t>(interval.start));
        builder.PushNonNullUnchecked(
            1, static_cast<int64_t>(interval.end - interval.start));
        builder.PushNonNullUnchecked(2, static_cast<int64_t>(interval.id));

        // Agg columns
        if (i < partition.agg_data.size()) {
          const auto& agg_row = partition.agg_data[i];
          for (size_t j = 0; j < agg_row.size(); ++j) {
            uint32_t col_offset = 3 + static_cast<uint32_t>(j);
            const auto& val = agg_row[j];
            if (val.type == SqlValue::kLong) {
              builder.PushNonNullUnchecked(col_offset, val.long_value);
            } else if (val.type == SqlValue::kDouble) {
              builder.PushNonNullUnchecked(col_offset, val.double_value);
            } else {
              builder.PushNull(col_offset, 1);
            }
          }
        } else {
          // Should not happen if data is consistent
          for (size_t j = 0; j < table->agg_column_names.size(); ++j) {
            builder.PushNull(3 + static_cast<uint32_t>(j), 1);
          }
        }

        // Partition columns
        uint32_t part_col_start =
            3 + static_cast<uint32_t>(table->agg_column_names.size());
        for (size_t j = 0; j < partition.sql_values.size(); ++j) {
          uint32_t col_idx = part_col_start + static_cast<uint32_t>(j);
          const auto& part_val = partition.sql_values[j];

          switch (part_val.type) {
            case SqlValue::kLong:
              builder.PushNonNullUnchecked(col_idx, part_val.long_value);
              break;
            case SqlValue::kDouble:
              builder.PushNonNullUnchecked(col_idx, part_val.double_value);
              break;
            case SqlValue::kString:
              builder.PushNonNullUnchecked(col_idx, partition_string_ids[j]);
              break;
            case SqlValue::kNull:
              builder.PushNull(col_idx, 1);
              break;
            case SqlValue::kBytes:
              PERFETTO_FATAL("Invalid partition type");
          }
        }
      }
    }

    SQLITE_ASSIGN_OR_RETURN(ctx, dataframe::Dataframe ret_tab,
                            std::move(builder).Build());
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(ret_tab)),
        "TABLE");
  }
};

}  // namespace

base::Status RegisterIntervalSelfIntersectFunctions(PerfettoSqlEngine& engine,
                                                    StringPool* pool) {
  base::Status status = engine.RegisterFunction<IntervalAggFn>(nullptr);
  if (!status.ok())
    return status;

  status = engine.RegisterFunction<IntervalSelfIntersect>(
      std::make_unique<IntervalSelfIntersect::UserData>(
          IntervalSelfIntersect::UserData{&engine, pool}));
  if (!status.ok())
    return status;

  return engine.RegisterFunction<IntervalsToTable>(
      std::make_unique<IntervalsToTable::UserData>(
          IntervalsToTable::UserData{&engine, pool}));
}

}  // namespace perfetto::trace_processor::perfetto_sql
