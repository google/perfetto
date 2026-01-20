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

// Event for sweep line algorithm
struct Event {
  int64_t ts;
  uint32_t id;
  bool is_start;

  bool operator<(const Event& other) const {
    if (ts != other.ts)
      return ts < other.ts;
    // Process starts before ends at the same timestamp
    return is_start > other.is_start;
  }
};

// Aggregation type
enum class AggType {
  kCount,
  kSum,
  kMin,
  kMax,
  kAvg,
};

// Column to aggregate
struct AggColumn {
  std::string name;
  AggType type;
  uint32_t col_idx;  // Index in the input table
};

// Parse aggregation specification like "count", "sum:value", "max:priority"
base::StatusOr<std::vector<AggColumn>> ParseAggregations(
    const std::string& agg_spec) {
  std::vector<AggColumn> aggs;
  if (agg_spec.empty()) {
    return aggs;
  }

  std::vector<std::string> parts = base::SplitString(agg_spec, ",");

  for (const auto& part : parts) {
    std::string trimmed = base::TrimWhitespace(part);
    if (trimmed.empty())
      continue;

    std::vector<std::string> agg_parts = base::SplitString(trimmed, ":");
    if (agg_parts.empty() || agg_parts.size() > 2) {
      return base::ErrStatus("Invalid aggregation spec: %s", trimmed.c_str());
    }

    AggColumn agg;
    std::string agg_type_str = base::TrimWhitespace(agg_parts[0]);

    if (agg_type_str == "count") {
      agg.type = AggType::kCount;
      agg.name = "count";
      agg.col_idx = 0;  // Count doesn't use column data
    } else if (agg_parts.size() != 2) {
      return base::ErrStatus(
          "Aggregation '%s' requires column name (e.g., sum:value)",
          agg_type_str.c_str());
    } else {
      agg.name = base::TrimWhitespace(agg_parts[1]);
      if (agg_type_str == "sum") {
        agg.type = AggType::kSum;
      } else if (agg_type_str == "min") {
        agg.type = AggType::kMin;
      } else if (agg_type_str == "max") {
        agg.type = AggType::kMax;
      } else if (agg_type_str == "avg") {
        agg.type = AggType::kAvg;
      } else {
        return base::ErrStatus("Unknown aggregation type: %s",
                               agg_type_str.c_str());
      }
      // For non-count aggregations, col_idx is the index in the agg_data array
      // Since we only support one aggregation column currently, it's always 0
      agg.col_idx = 0;
    }

    aggs.push_back(std::move(agg));
  }

  return aggs;
}

// Aggregator for a single bucket
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

// Process a single partition and compute self-intersections
base::StatusOr<uint32_t> ProcessPartition(
    StringPool* string_pool,
    dataframe::AdhocDataframeBuilder& builder,
    const Partition& partition,
    const std::vector<AggColumn>& agg_columns,
    const std::vector<std::vector<SqlValue>>& interval_data) {
  if (partition.intervals.empty()) {
    return 0;
  }

  // Build events for sweep line algorithm
  std::vector<Event> events;
  events.reserve(partition.intervals.size() * 2);

  for (const auto& interval : partition.intervals) {
    events.push_back(Event{static_cast<int64_t>(interval.start),
                           static_cast<uint32_t>(interval.id), true});
    events.push_back(Event{static_cast<int64_t>(interval.end),
                           static_cast<uint32_t>(interval.id), false});
  }

  std::sort(events.begin(), events.end());

  // Use bitset for dense IDs (assuming IDs are relatively small and dense)
  // For very large or sparse IDs, we'd fall back to a set
  uint32_t max_id = 0;
  for (const auto& interval : partition.intervals) {
    max_id = std::max(max_id, static_cast<uint32_t>(interval.id));
  }

  // Use vector<bool> as bitset for dense IDs
  constexpr uint32_t kMaxDenseBitsetSize = 100000;
  std::vector<bool> active_bitset;
  base::FlatHashMap<uint32_t, bool> active_map;
  bool use_bitset = max_id < kMaxDenseBitsetSize;

  if (use_bitset) {
    active_bitset.resize(max_id + 1, false);
  }

  // Track active IDs for efficient iteration (FIX #2)
  std::vector<uint32_t> active_ids;
  active_ids.reserve(partition.intervals.size());
  uint32_t active_count = 0;  // FIX #1: Maintain count instead of recounting

  uint32_t rows_pushed = 0;
  int64_t prev_ts = events[0].ts;
  uint32_t group_id = 0;
  uint32_t prev_active_count = 0;  // Track previous active count

  BucketAggregator agg;
  agg.Reset(agg_columns.size());

  // Pre-intern partition strings once (FIX #4)
  std::vector<StringPool::Id> partition_string_ids;
  partition_string_ids.reserve(partition.sql_values.size());
  for (const auto& part_val : partition.sql_values) {
    if (part_val.type == SqlValue::kString) {
      partition_string_ids.push_back(
          string_pool->InternString(part_val.string_value));
    } else {
      partition_string_ids.push_back(StringPool::Id::Null());
    }
  }

  auto emit_bucket = [&](int64_t start_ts, int64_t end_ts) {
    if (start_ts >= end_ts)
      return;

    // FIX #1: Use maintained count instead of recounting
    if (active_count == 0)
      return;

    // Compute aggregations
    agg.Reset(agg_columns.size());
    agg.count = active_count;

    // FIX #2: Iterate only active IDs instead of all possible IDs
    if (!agg_columns.empty() && !interval_data.empty()) {
      for (uint32_t id : active_ids) {
        if (id < interval_data.size() && !interval_data[id].empty()) {
          for (size_t agg_idx = 0; agg_idx < agg_columns.size(); ++agg_idx) {
            const auto& agg_col = agg_columns[agg_idx];
            if (agg_col.type != AggType::kCount) {
              // agg_col.col_idx is the index within the aggregation columns
              // interval_data[id] contains the aggregation column values for
              // this interval
              if (agg_col.col_idx < interval_data[id].size()) {
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
    }

    // Push row: ts, dur, group_id, [aggregations...]
    builder.PushNonNullUnchecked(0, start_ts);
    builder.PushNonNullUnchecked(1, end_ts - start_ts);
    builder.PushNonNullUnchecked(2, static_cast<int64_t>(group_id));

    // Push aggregation results
    for (size_t agg_idx = 0; agg_idx < agg_columns.size(); ++agg_idx) {
      const auto& agg_col = agg_columns[agg_idx];
      uint32_t col_offset = 3 + static_cast<uint32_t>(agg_idx);

      switch (agg_col.type) {
        case AggType::kCount:
          builder.PushNonNullUnchecked(col_offset,
                                       static_cast<int64_t>(agg.count));
          break;
        case AggType::kSum:
          builder.PushNonNullUnchecked(col_offset, agg.sums[agg_idx]);
          break;
        case AggType::kMin:
          if (agg.mins[agg_idx]) {
            builder.PushNonNullUnchecked(col_offset, *agg.mins[agg_idx]);
          } else {
            builder.PushNull(col_offset, 1);
          }
          break;
        case AggType::kMax:
          if (agg.maxs[agg_idx]) {
            builder.PushNonNullUnchecked(col_offset, *agg.maxs[agg_idx]);
          } else {
            builder.PushNull(col_offset, 1);
          }
          break;
        case AggType::kAvg:
          if (agg.count > 0) {
            builder.PushNonNullUnchecked(col_offset,
                                         agg.sums[agg_idx] / agg.count);
          } else {
            builder.PushNull(col_offset, 1);
          }
          break;
      }
    }

    // Push partition columns (FIX #4: Use pre-interned strings)
    for (size_t i = 0; i < partition.sql_values.size(); ++i) {
      const SqlValue& part_val = partition.sql_values[i];
      uint32_t col_idx = 3 + static_cast<uint32_t>(agg_columns.size()) +
                         static_cast<uint32_t>(i);
      switch (part_val.type) {
        case SqlValue::kLong:
          if (!builder.PushNonNull(col_idx, part_val.long_value, 1)) {
            return;
          }
          break;
        case SqlValue::kDouble:
          if (!builder.PushNonNull(col_idx, part_val.double_value, 1)) {
            return;
          }
          break;
        case SqlValue::kString:
          if (!builder.PushNonNull(col_idx, partition_string_ids[i], 1)) {
            return;
          }
          break;
        case SqlValue::kNull:
          builder.PushNull(col_idx, 1);
          break;
        case SqlValue::kBytes:
          PERFETTO_FATAL("Invalid partition type");
      }
    }

    rows_pushed++;
  };

  // Sweep line algorithm
  for (const auto& event : events) {
    // Emit bucket for previous timestamp range if timestamp changed
    if (event.ts > prev_ts) {
      emit_bucket(prev_ts, event.ts);
      group_id++;
      prev_ts = event.ts;
    }

    // Update active set (FIX #1 & #2: Maintain active_count and active_ids)
    uint32_t old_active_count = active_count;

    if (event.is_start) {
      if (use_bitset) {
        if (!active_bitset[event.id]) {
          active_bitset[event.id] = true;
          active_ids.push_back(event.id);
          active_count++;
        }
      } else {
        if (!active_map.Find(event.id)) {
          active_map[event.id] = true;
          active_ids.push_back(event.id);
          active_count++;
        }
      }
    } else {
      if (use_bitset) {
        if (active_bitset[event.id]) {
          active_bitset[event.id] = false;
          // Remove from active_ids
          auto it = std::find(active_ids.begin(), active_ids.end(), event.id);
          if (it != active_ids.end()) {
            active_ids.erase(it);
          }
          active_count--;
        }
      } else {
        if (active_map.Find(event.id)) {
          active_map.Erase(event.id);
          // Remove from active_ids
          auto it = std::find(active_ids.begin(), active_ids.end(), event.id);
          if (it != active_ids.end()) {
            active_ids.erase(it);
          }
          active_count--;
        }
      }
    }

    // If active count changed at the same timestamp, we need to track this
    // for the next timestamp change
    if (active_count != old_active_count) {
      prev_active_count = old_active_count;
    }
  }

  return rows_pushed;
}

struct IntervalSelfIntersect : public sqlite::Function<IntervalSelfIntersect> {
  static constexpr char kName[] = "__intrinsic_interval_self_intersect";
  static constexpr int kArgCount = -1;

  struct UserData {
    PerfettoSqlEngine* engine;
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc < 2 || argc > 3) {
      return sqlite::result::Error(
          ctx,
          "interval_self_intersect: Expected 2-3 arguments (table, "
          "partition_cols, [agg_spec])");
    }

    const char* partition_list = sqlite::value::Text(argv[1]);
    if (!partition_list) {
      return sqlite::result::Error(
          ctx, "interval_self_intersect: partition column list cannot be null");
    }

    // Parse aggregation spec (optional third argument)
    std::string agg_spec;
    if (argc >= 3) {
      const char* agg_spec_str = sqlite::value::Text(argv[2]);
      if (agg_spec_str) {
        agg_spec = agg_spec_str;
      }
    }

    SQLITE_ASSIGN_OR_RETURN(ctx, std::vector<AggColumn> agg_columns,
                            ParseAggregations(agg_spec));

    // Get column names for result
    std::vector<std::string> ret_col_names{"ts", "dur", "group_id"};

    // Add aggregation column names
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
      ret_col_names.push_back(col_name);
    }

    // Add partition column names
    std::vector<std::string> partition_columns =
        base::SplitString(base::StripChars(partition_list, "()", ' '), ",");
    for (const auto& c : partition_columns) {
      std::string p_col_name = base::TrimWhitespace(c);
      if (!p_col_name.empty()) {
        ret_col_names.push_back(p_col_name);
      }
    }

    // Get input table
    PartitionedTable* table = sqlite::value::Pointer<PartitionedTable>(
        argv[0], PartitionedTable::kName);

    if (!table || table->partitions_map.size() == 0) {
      std::vector<ColType> col_types(ret_col_names.size(), ColType::kInt64);
      dataframe::AdhocDataframeBuilder builder(
          ret_col_names, GetUserData(ctx)->pool, col_types);
      SQLITE_ASSIGN_OR_RETURN(ctx, dataframe::Dataframe ret_table,
                              std::move(builder).Build());
      return sqlite::result::UniquePointer(
          ctx, std::make_unique<dataframe::Dataframe>(std::move(ret_table)),
          "TABLE");
    }

    // Build column types
    std::vector<ColType> col_types{ColType::kInt64, ColType::kInt64,
                                   ColType::kInt64};  // ts, dur, group_id

    // Add aggregation column types
    for (const auto& agg : agg_columns) {
      if (agg.type == AggType::kCount) {
        col_types.push_back(ColType::kInt64);
      } else {
        col_types.push_back(ColType::kDouble);
      }
    }

    // Add partition column types
    auto partition_it = table->partitions_map.GetIterator();
    if (partition_it) {
      for (const auto& val : partition_it.value().sql_values) {
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

    // Process each partition
    for (auto p_it = table->partitions_map.GetIterator(); p_it; ++p_it) {
      // Use aggregation data from the partition
      const auto& interval_data = p_it.value().agg_data;

      auto status_or_rows =
          ProcessPartition(GetUserData(ctx)->pool, builder, p_it.value(),
                           agg_columns, interval_data);
      if (!status_or_rows.ok()) {
        return sqlite::result::Error(ctx, status_or_rows.status().c_message());
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
  return engine.RegisterFunction<IntervalSelfIntersect>(
      std::make_unique<IntervalSelfIntersect::UserData>(
          IntervalSelfIntersect::UserData{&engine, pool}));
}

}  // namespace perfetto::trace_processor::perfetto_sql
