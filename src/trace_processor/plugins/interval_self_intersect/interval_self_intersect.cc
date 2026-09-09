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

#include "src/trace_processor/plugins/interval_self_intersect/interval_self_intersect.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/partitioned_intervals.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::interval_self_intersect {
namespace {

using ColType = dataframe::AdhocDataframeBuilder::ColumnType;
using perfetto_sql::PartitionedTable;

// Sweep-line event: an interval start or end at a given timestamp.
// Tie-break: starts before ends at identical ts so an interval that ends
// exactly when another starts doesn't briefly drop the active set.
struct Event {
  int64_t ts;
  uint32_t id;
  int8_t delta;  // +1 = start, -1 = end

  bool operator<(const Event& other) const {
    if (ts != other.ts) {
      return ts < other.ts;
    }
    return delta > other.delta;
  }
};

constexpr uint32_t kMaxDenseBitsetSize = 100000;

// Mirror the old SQL intervals.intersect.interval_self_intersect macro's
// output exactly: one row per (atomic segment × active interval), plus one
// "end marker" row per interval at the segment that begins at the interval's
// end ts (dur taken from that segment, so the last interval emits a dur=0
// end marker at the final endpoint).
//
// Implementation: single O(n log n) sweep over all events from the input.
// Partition columns on the input (if any) are ignored — the macro contract
// matches the partitionless SQL macro, callers can JOIN back to the source
// to recover any per-interval attributes.
void RunSelfIntersect(dataframe::AdhocDataframeBuilder& builder,
                      const PartitionedTable& table) {
  size_t total_intervals = 0;
  for (auto p_it = table.partitions_map.GetIterator(); p_it; ++p_it) {
    total_intervals += p_it.value().intervals.size();
  }
  if (total_intervals == 0) {
    return;
  }

  std::vector<Event> events;
  events.reserve(total_intervals * 2);
  uint32_t max_id = 0;
  for (auto p_it = table.partitions_map.GetIterator(); p_it; ++p_it) {
    for (const auto& iv : p_it.value().intervals) {
      events.push_back(Event{static_cast<int64_t>(iv.start),
                             static_cast<uint32_t>(iv.id),
                             static_cast<int8_t>(1)});
      events.push_back(Event{static_cast<int64_t>(iv.end),
                             static_cast<uint32_t>(iv.id),
                             static_cast<int8_t>(-1)});
      max_id = std::max(max_id, static_cast<uint32_t>(iv.id));
    }
  }
  std::sort(events.begin(), events.end());

  // Active set: bitset for dense ids, FlatHashMap fallback. active_ids holds
  // ids in insertion order so emit_segment can iterate without re-scanning.
  const bool use_bitset = max_id < kMaxDenseBitsetSize;
  std::vector<bool> active_bitset;
  base::FlatHashMap<uint32_t, bool> active_map;
  if (use_bitset) {
    active_bitset.resize(max_id + 1, false);
  }
  std::vector<uint32_t> active_ids;

  // End ids seen at prev_ts — emitted into the next segment (the one that
  // begins at their end ts), matching the old SQL macro's "end marker"
  // attribution.
  std::vector<uint32_t> ends_at_current;

  uint32_t group_id = 1;
  int64_t prev_ts = events.front().ts;

  auto emit_segment = [&](int64_t seg_ts, int64_t next_ts) {
    int64_t seg_dur = next_ts - seg_ts;
    for (uint32_t id : active_ids) {
      builder.PushNonNullUnchecked(0, seg_ts);
      builder.PushNonNullUnchecked(1, seg_dur);
      builder.PushNonNullUnchecked(2, static_cast<int64_t>(group_id));
      builder.PushNonNullUnchecked(3, static_cast<int64_t>(id));
      builder.PushNonNullUnchecked(4, static_cast<int64_t>(0));
    }
    for (uint32_t id : ends_at_current) {
      builder.PushNonNullUnchecked(0, seg_ts);
      builder.PushNonNullUnchecked(1, seg_dur);
      builder.PushNonNullUnchecked(2, static_cast<int64_t>(group_id));
      builder.PushNonNullUnchecked(3, static_cast<int64_t>(id));
      builder.PushNonNullUnchecked(4, static_cast<int64_t>(1));
    }
  };

  for (const auto& ev : events) {
    if (ev.ts > prev_ts) {
      emit_segment(prev_ts, ev.ts);
      ends_at_current.clear();
      ++group_id;
      prev_ts = ev.ts;
    }
    if (ev.delta > 0) {
      bool already;
      if (use_bitset) {
        already = active_bitset[ev.id];
        if (!already) {
          active_bitset[ev.id] = true;
        }
      } else {
        already = active_map.Find(ev.id) != nullptr;
        if (!already) {
          active_map[ev.id] = true;
        }
      }
      if (!already) {
        active_ids.push_back(ev.id);
      }
    } else {
      if (use_bitset) {
        active_bitset[ev.id] = false;
      } else {
        active_map.Erase(ev.id);
      }
      auto it = std::find(active_ids.begin(), active_ids.end(), ev.id);
      if (it != active_ids.end()) {
        active_ids.erase(it);
      }
      ends_at_current.push_back(ev.id);
    }
  }

  // Final segment at the last endpoint: dur=0, no active rows, end markers
  // for the intervals that closed at this ts.
  emit_segment(prev_ts, prev_ts);
}

struct IntervalSelfIntersect : public sqlite::Function<IntervalSelfIntersect> {
  static constexpr char kName[] = "__intrinsic_interval_self_intersect";
  static constexpr int kArgCount = 1;

  struct UserData {
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);

    std::vector<std::string> ret_col_names{"ts", "dur", "group_id", "id",
                                           "interval_ends_at_ts"};
    std::vector<ColType> col_types{ColType::kInt64, ColType::kInt64,
                                   ColType::kInt64, ColType::kInt64,
                                   ColType::kInt64};

    PartitionedTable* table = sqlite::value::Pointer<PartitionedTable>(
        argv[0], PartitionedTable::kName);

    dataframe::AdhocDataframeBuilder builder(
        ret_col_names, GetUserData(ctx)->pool,
        dataframe::AdhocDataframeBuilder::Options{
            col_types, dataframe::NullabilityType::kDenseNull});

    if (table && table->partitions_map.size() != 0) {
      RunSelfIntersect(builder, *table);
    }

    SQLITE_ASSIGN_OR_RETURN(ctx, dataframe::Dataframe ret_tab,
                            std::move(builder).Build());
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(ret_tab)),
        "TABLE");
  }
};

// ---------------------------------------------------------------------------
// Partitioned self-intersect with direct aggregation.
//
// Unlike the drop-in variant above (which mirrors the SQL macro's
// per-(segment x active interval) output), this pipeline aggregates DURING
// the sweep and emits exactly one row per atomic segment per partition:
//
//   ts, dur, group_id, cnt, sum_value, min_value, max_value, <partitions...>
//
// so the output size is bounded by 2x the input row count per partition set,
// independent of how many intervals overlap. Zero-active gap segments are
// emitted too (cnt = 0), including a final dur=0 segment at each partition's
// last endpoint, so a counter track sourced from this table drops to zero
// exactly where the old end-marker rows made it drop.
// ---------------------------------------------------------------------------

// Max partition columns; must match the c7..c21 binds in
// stdlib/intervals/self_intersect.sql.
constexpr uint32_t kMaxPartitionCols = 15;

// Input blob built by __intrinsic_isi_intervals_agg and consumed by
// __intrinsic_interval_self_intersect_agg. Self-contained (not shared with
// the interval_intersect machinery): intervals are stored in insertion order
// per partition, never indexed by any caller-supplied id.
struct IsiIntervals {
  static constexpr char kName[] = "ISI_INTERVALS";

  struct Interval {
    int64_t start;
    int64_t end;
    double value;  // Meaningless when !has_value.
    bool has_value;
  };

  struct Part {
    // Partition column values; strings interned in the StringPool.
    std::vector<SqlValue> key;
    std::vector<Interval> intervals;
  };

  std::vector<std::string> partition_col_names;
  // Keyed by murmur hash of the partition tuple. NULL hashes like a regular
  // value, so NULL partition keys form their own partition (no rows are ever
  // dropped on NULL, unlike SQL equality joins).
  base::FlatHashMap<uint64_t, Part> parts;
};

inline void HashSqlValue(base::MurmurHashCombiner& h, const SqlValue& v) {
  h.Combine(v.type);
  switch (v.type) {
    case SqlValue::Type::kString:
      h.Combine(base::StringView(v.AsString()));
      break;
    case SqlValue::Type::kDouble:
      h.Combine(v.AsDouble());
      break;
    case SqlValue::Type::kLong:
      h.Combine(v.AsLong());
      break;
    case SqlValue::Type::kNull:
      h.Combine(0);
      break;
    case SqlValue::Type::kBytes:
      PERFETTO_FATAL("Wrong type");
  }
}

// __intrinsic_isi_intervals_agg(ts, dur, value, [part_name, part_val]*)
//
// Collects intervals into an IsiIntervals blob. Every input row is one
// interval; `value` is the aggregation value (NULL for callers that only
// need counts). Input does NOT need to be sorted — the sweep sorts events
// itself.
struct IsiIntervalsAgg : public sqlite::AggregateFunction<IsiIntervalsAgg> {
  static constexpr char kName[] = "__intrinsic_isi_intervals_agg";
  static constexpr int kArgCount = -1;
  static constexpr uint32_t kMinArgCount = 3;

  struct UserData {
    StringPool* pool;
  };

  struct AggCtx : sqlite::AggregateContext<AggCtx> {
    IsiIntervals intervals;
    std::vector<SqlValue> tmp_key;
    bool cols_initialized = false;
  };

  static void Step(sqlite3_context* ctx, int rargc, sqlite3_value** argv) {
    auto argc = static_cast<uint32_t>(rargc);
    if (argc < kMinArgCount || (argc - kMinArgCount) % 2 != 0) {
      return sqlite::result::Error(
          ctx,
          "isi_intervals_agg: expected (ts, dur, value, [part_name, "
          "part_val]*)");
    }
    auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);

    if (!agg_ctx.cols_initialized) {
      uint32_t part_count = (argc - kMinArgCount) / 2;
      if (part_count > kMaxPartitionCols) {
        return sqlite::result::Error(
            ctx, "isi_intervals_agg: at most 15 partition columns supported");
      }
      for (uint32_t i = kMinArgCount; i < argc; i += 2) {
        agg_ctx.intervals.partition_col_names.push_back(
            sqlite::utils::SqliteValueToSqlValue(argv[i]).AsString());
      }
      agg_ctx.tmp_key.resize(part_count);
      agg_ctx.cols_initialized = true;
    }

    int64_t ts = sqlite::value::Int64(argv[0]);
    int64_t dur = sqlite::value::Int64(argv[1]);
    if (dur < 0) {
      return sqlite::result::Error(
          ctx, "isi_intervals_agg: negative durations are not supported");
    }
    if (ts > std::numeric_limits<int64_t>::max() - dur) {
      return sqlite::result::Error(ctx,
                                   "isi_intervals_agg: ts + dur overflows");
    }

    IsiIntervals::Interval interval;
    interval.start = ts;
    interval.end = ts + dur;
    SqlValue value = sqlite::utils::SqliteValueToSqlValue(argv[2]);
    switch (value.type) {
      case SqlValue::Type::kLong:
        interval.value = static_cast<double>(value.long_value);
        interval.has_value = true;
        break;
      case SqlValue::Type::kDouble:
        interval.value = value.double_value;
        interval.has_value = true;
        break;
      case SqlValue::Type::kNull:
        interval.value = 0;
        interval.has_value = false;
        break;
      case SqlValue::Type::kString:
      case SqlValue::Type::kBytes:
        return sqlite::result::Error(
            ctx, "isi_intervals_agg: value must be numeric or NULL");
    }

    base::MurmurHashCombiner h;
    StringPool* pool = GetUserData(ctx)->pool;
    for (uint32_t i = 0; i < agg_ctx.tmp_key.size(); ++i) {
      SqlValue v =
          sqlite::utils::SqliteValueToSqlValue(argv[kMinArgCount + i * 2 + 1]);
      if (v.type == SqlValue::kString) {
        // Intern so the pointer outlives this Step call and duplicates share
        // storage.
        v.string_value = pool->Get(pool->InternString(v.AsString())).c_str();
      }
      agg_ctx.tmp_key[i] = v;
      HashSqlValue(h, v);
    }

    auto& parts = agg_ctx.intervals.parts;
    IsiIntervals::Part* part = parts.Find(h.digest());
    if (!part) {
      IsiIntervals::Part new_part;
      new_part.key = agg_ctx.tmp_key;
      part = parts.Insert(h.digest(), std::move(new_part)).first;
    }
    part->intervals.push_back(interval);
  }

  static void Final(sqlite3_context* ctx) {
    auto raw = AggCtx::GetContextOrNullForFinal(ctx);
    if (!raw) {
      return sqlite::result::Null(ctx);
    }
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<IsiIntervals>(std::move(raw.get()->intervals)),
        IsiIntervals::kName);
  }
};

// Bit-level double equality for the segment-merge check: merging must only
// happen when the emitted value would be byte-identical, and bit comparison
// sidesteps float-equality pitfalls (NaN never equals itself with ==; a
// missed merge just emits an extra, still-correct row).
inline bool BitEqual(double a, double b) {
  uint64_t ia;
  uint64_t ib;
  memcpy(&ia, &a, sizeof(ia));
  memcpy(&ib, &b, sizeof(ib));
  return ia == ib;
}

// Output column layout; must match the c0..c6 binds in
// stdlib/intervals/self_intersect.sql.
constexpr uint32_t kOutTs = 0;
constexpr uint32_t kOutDur = 1;
constexpr uint32_t kOutGroupId = 2;
constexpr uint32_t kOutCnt = 3;
constexpr uint32_t kOutSum = 4;
constexpr uint32_t kOutMin = 5;
constexpr uint32_t kOutMax = 6;
constexpr uint32_t kOutPartitionOffset = 7;

// Runs the aggregating sweep for one partition, appending one row per atomic
// segment. Returns the number of rows emitted.
uint32_t SweepPartition(dataframe::AdhocDataframeBuilder& builder,
                        const IsiIntervals::Part& part,
                        int64_t& group_id) {
  struct AggEvent {
    int64_t ts;
    int8_t delta;  // +1 = start, -1 = end
    double value;
    bool has_value;

    // Starts sort before ends at the same ts so a zero-duration interval's
    // value is inserted into the aggregation state before it is erased.
    bool operator<(const AggEvent& other) const {
      if (ts != other.ts) {
        return ts < other.ts;
      }
      return delta > other.delta;
    }
  };

  std::vector<AggEvent> events;
  events.reserve(part.intervals.size() * 2);
  for (const auto& iv : part.intervals) {
    events.push_back(AggEvent{iv.start, 1, iv.value, iv.has_value});
    events.push_back(AggEvent{iv.end, -1, iv.value, iv.has_value});
  }
  std::sort(events.begin(), events.end());

  // Incremental aggregation state. cnt/sum are O(1) per event; min/max use a
  // multiset for O(log n) insert/erase, keeping the whole sweep O(n log n)
  // with no per-segment rescan of the active set.
  int64_t cnt = 0;
  double sum = 0;
  std::multiset<double> values;

  // Adjacent atomic segments with identical aggregates are merged into one
  // row: a boundary where as many intervals start as end (e.g. back-to-back
  // slices) changes nothing a step-function consumer can observe, so emitting
  // it would only inflate the output. The pending row is extended until the
  // aggregates change, then flushed. Merging on the exact double `sum` is
  // conservative: a bit-level difference just emits an extra (still correct)
  // row.
  bool pending = false;
  int64_t pend_ts = 0;
  int64_t pend_end = 0;
  int64_t pend_cnt = 0;
  double pend_sum = 0;
  bool pend_has_minmax = false;
  double pend_min = 0;
  double pend_max = 0;

  uint32_t rows = 0;
  auto flush = [&]() {
    if (!pending) {
      return;
    }
    builder.PushNonNullUnchecked(kOutTs, pend_ts);
    builder.PushNonNullUnchecked(kOutDur, pend_end - pend_ts);
    builder.PushNonNullUnchecked(kOutGroupId, group_id);
    builder.PushNonNullUnchecked(kOutCnt, pend_cnt);
    builder.PushNonNullUnchecked(kOutSum, pend_sum);
    if (pend_has_minmax) {
      builder.PushNonNullUnchecked(kOutMin, pend_min);
      builder.PushNonNullUnchecked(kOutMax, pend_max);
    } else {
      builder.PushNull(kOutMin, 1);
      builder.PushNull(kOutMax, 1);
    }
    ++group_id;
    ++rows;
    pending = false;
  };

  auto add_segment = [&](int64_t seg_ts, int64_t seg_end) {
    bool has_minmax = !values.empty();
    double mn = has_minmax ? *values.begin() : 0;
    double mx = has_minmax ? *values.rbegin() : 0;
    // Segments within a partition are contiguous (pend_end == seg_ts always
    // holds); the check documents the merge invariant.
    if (pending && pend_end == seg_ts && pend_cnt == cnt &&
        BitEqual(pend_sum, sum) && pend_has_minmax == has_minmax &&
        (!has_minmax || (BitEqual(pend_min, mn) && BitEqual(pend_max, mx)))) {
      pend_end = seg_end;
      return;
    }
    flush();
    pending = true;
    pend_ts = seg_ts;
    pend_end = seg_end;
    pend_cnt = cnt;
    pend_sum = sum;
    pend_has_minmax = has_minmax;
    pend_min = mn;
    pend_max = mx;
  };

  int64_t prev_ts = events.front().ts;
  for (const auto& ev : events) {
    if (ev.ts > prev_ts) {
      // Add the segment [prev_ts, ev.ts) with the state accumulated from
      // all events at prev_ts and earlier. Zero-active gaps count too.
      add_segment(prev_ts, ev.ts);
      prev_ts = ev.ts;
    }
    if (ev.delta > 0) {
      ++cnt;
      if (ev.has_value) {
        sum += ev.value;
        values.insert(ev.value);
      }
    } else {
      --cnt;
      if (ev.has_value) {
        sum -= ev.value;
        values.erase(values.find(ev.value));
      }
    }
  }
  // Every start has a matching end, so the active set is empty here; the
  // trailing zero-width segment is the counter's drop-to-zero point at the
  // partition's last endpoint (it merges into a preceding zero-count gap
  // row when one exists).
  PERFETTO_DCHECK(cnt == 0);
  add_segment(prev_ts, prev_ts);
  flush();
  return rows;
}

// __intrinsic_interval_self_intersect_agg(intervals_ptr, partition_list)
//
// Consumes an IsiIntervals blob and emits the aggregated atomic segments as
// a dataframe. `partition_list` is the stringified partition column list
// (e.g. '(k0, k1)'); it names the output columns so the schema is stable
// even when the input is empty, and it is validated against the blob.
struct IntervalSelfIntersectAgg
    : public sqlite::Function<IntervalSelfIntersectAgg> {
  static constexpr char kName[] = "__intrinsic_interval_self_intersect_agg";
  static constexpr int kArgCount = 2;

  struct UserData {
    StringPool* pool;
  };

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == kArgCount);

    const char* partition_list = sqlite::value::Text(argv[1]);
    if (!partition_list) {
      return sqlite::result::Error(
          ctx, "interval_self_intersect_agg: column list cannot be null");
    }
    std::vector<std::string> ret_col_names{
        "ts", "dur", "group_id", "cnt", "sum_value", "min_value", "max_value"};
    std::vector<ColType> col_types{
        ColType::kInt64,  ColType::kInt64,  ColType::kInt64, ColType::kInt64,
        ColType::kDouble, ColType::kDouble, ColType::kDouble};
    uint32_t num_partition_cols = 0;
    for (const auto& c :
         base::SplitString(base::StripChars(partition_list, "()", ' '), ",")) {
      std::string name = base::TrimWhitespace(c);
      if (!name.empty()) {
        ret_col_names.push_back(name);
        ++num_partition_cols;
      }
    }
    if (num_partition_cols > kMaxPartitionCols) {
      return sqlite::result::Error(
          ctx,
          "interval_self_intersect_agg: at most 15 partition columns "
          "supported");
    }

    auto* intervals =
        sqlite::value::Pointer<IsiIntervals>(argv[0], IsiIntervals::kName);
    if (intervals &&
        intervals->partition_col_names.size() != num_partition_cols) {
      return sqlite::result::Error(
          ctx,
          "interval_self_intersect_agg: partition list does not match the "
          "columns the intervals were collected with");
    }

    // Partition column types: from the first non-NULL value per column
    // across partitions; a column that is NULL everywhere defaults to int64
    // (only nulls are ever pushed into it).
    StringPool* pool = GetUserData(ctx)->pool;
    std::vector<ColType> part_types(num_partition_cols, ColType::kInt64);
    if (intervals) {
      std::vector<bool> known(num_partition_cols, false);
      uint32_t remaining = num_partition_cols;
      for (auto it = intervals->parts.GetIterator(); it && remaining; ++it) {
        for (uint32_t i = 0; i < num_partition_cols; ++i) {
          if (known[i] || it.value().key[i].is_null()) {
            continue;
          }
          switch (it.value().key[i].type) {
            case SqlValue::kLong:
              part_types[i] = ColType::kInt64;
              break;
            case SqlValue::kDouble:
              part_types[i] = ColType::kDouble;
              break;
            case SqlValue::kString:
              part_types[i] = ColType::kString;
              break;
            case SqlValue::kNull:
            case SqlValue::kBytes:
              PERFETTO_FATAL("Unreachable");
          }
          known[i] = true;
          --remaining;
        }
      }
    }
    col_types.insert(col_types.end(), part_types.begin(), part_types.end());

    dataframe::AdhocDataframeBuilder builder(
        ret_col_names, pool,
        dataframe::AdhocDataframeBuilder::Options{
            col_types, dataframe::NullabilityType::kDenseNull});

    int64_t group_id = 1;
    if (intervals) {
      for (auto it = intervals->parts.GetIterator(); it; ++it) {
        const IsiIntervals::Part& part = it.value();
        if (part.intervals.empty()) {
          continue;
        }
        uint32_t rows = SweepPartition(builder, part, group_id);
        for (uint32_t i = 0; i < num_partition_cols; ++i) {
          const SqlValue& v = part.key[i];
          bool ok = true;
          switch (v.type) {
            case SqlValue::kLong:
              ok = builder.PushNonNull(kOutPartitionOffset + i, v.long_value,
                                       rows);
              break;
            case SqlValue::kDouble:
              ok = builder.PushNonNull(kOutPartitionOffset + i, v.double_value,
                                       rows);
              break;
            case SqlValue::kString:
              ok =
                  builder.PushNonNull(kOutPartitionOffset + i,
                                      pool->InternString(v.string_value), rows);
              break;
            case SqlValue::kNull:
              builder.PushNull(kOutPartitionOffset + i, rows);
              break;
            case SqlValue::kBytes:
              PERFETTO_FATAL("Unreachable");
          }
          if (!ok) {
            return sqlite::result::Error(ctx, builder.status().c_message());
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

class IntervalSelfIntersectPlugin : public Plugin<IntervalSelfIntersectPlugin> {
 public:
  ~IntervalSelfIntersectPlugin() override;

  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeFunctionRegistration<IntervalSelfIntersect>(
        std::make_unique<IntervalSelfIntersect::UserData>(
            IntervalSelfIntersect::UserData{pool})));
    out.push_back(MakeFunctionRegistration<IntervalSelfIntersectAgg>(
        std::make_unique<IntervalSelfIntersectAgg::UserData>(
            IntervalSelfIntersectAgg::UserData{pool})));
  }

  void RegisterAggregateFunctions(
      PerfettoSqlConnection*,
      std::vector<AggregateFunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    isi_agg_user_data_ = std::make_unique<IsiIntervalsAgg::UserData>(
        IsiIntervalsAgg::UserData{pool});
    out.push_back(
        MakeAggregateRegistration<IsiIntervalsAgg>(isi_agg_user_data_.get()));
  }

 private:
  // MakeAggregateRegistration takes a non-owning pointer; the plugin instance
  // owns the user data for the lifetime of the connection.
  std::unique_ptr<IsiIntervalsAgg::UserData> isi_agg_user_data_;
};

IntervalSelfIntersectPlugin::~IntervalSelfIntersectPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<IntervalSelfIntersectPlugin>();
      },
      IntervalSelfIntersectPlugin::kPluginId,
      IntervalSelfIntersectPlugin::kDepIds.data(),
      IntervalSelfIntersectPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::interval_self_intersect
