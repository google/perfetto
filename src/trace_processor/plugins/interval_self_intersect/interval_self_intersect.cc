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
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/partitioned_intervals.h"
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
      events.push_back(
          Event{static_cast<int64_t>(iv.start), static_cast<uint32_t>(iv.id),
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

struct IntervalSelfIntersect
    : public sqlite::Function<IntervalSelfIntersect> {
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

class IntervalSelfIntersectPlugin
    : public Plugin<IntervalSelfIntersectPlugin> {
 public:
  ~IntervalSelfIntersectPlugin() override;

  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeFunctionRegistration<IntervalSelfIntersect>(
        std::make_unique<IntervalSelfIntersect::UserData>(
            IntervalSelfIntersect::UserData{pool})));
  }
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
