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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_GLOBAL_STATS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_GLOBAL_STATS_TRACKER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/tables/metadata_tables_py.h"

namespace perfetto::trace_processor {

// Tracks stats globally across all machines and traces.
//
// This is the central owner of all stats data. Per-context StatsTracker
// instances delegate to this class, passing their machine_id and trace_id.
// The scope of each stat (defined in stats.h) determines how the context
// ids are used for storage.
class GlobalStatsTracker {
 public:
  using MachineId = tables::MachineTable::Id;
  using TraceId = tables::TraceFileTable::Id;

  struct Stats {
    using IndexMap = std::map<int, int64_t>;
    int64_t value = 0;
    IndexMap indexed_values;
  };

  GlobalStatsTracker();

  void SetStats(std::optional<MachineId> machine_id,
                std::optional<TraceId> trace_id,
                size_t key,
                int64_t value);

  void IncrementStats(std::optional<MachineId> machine_id,
                      std::optional<TraceId> trace_id,
                      size_t key,
                      int64_t increment = 1);

  void SetIndexedStats(std::optional<MachineId> machine_id,
                       std::optional<TraceId> trace_id,
                       size_t key,
                       int index,
                       int64_t value);

  void IncrementIndexedStats(std::optional<MachineId> machine_id,
                             std::optional<TraceId> trace_id,
                             size_t key,
                             int index,
                             int64_t increment = 1);

  int64_t GetStats(std::optional<MachineId> machine_id,
                   std::optional<TraceId> trace_id,
                   size_t key);

  std::optional<int64_t> GetIndexedStats(std::optional<MachineId> machine_id,
                                         std::optional<TraceId> trace_id,
                                         size_t key,
                                         int index);

  // Represents a single context (machine_id, trace_id combination) that
  // has stats stored in it.
  struct ContextKey {
    std::optional<MachineId> machine_id;
    std::optional<TraceId> trace_id;

    bool operator==(const ContextKey& other) const {
      return machine_id == other.machine_id && trace_id == other.trace_id;
    }

    template <typename H>
    friend H PerfettoHashValue(H h, const ContextKey& value) {
      return H::Combine(std::move(h), value.machine_id, value.trace_id);
    }
  };

  using StatsMap = std::array<Stats, stats::kNumKeys>;

  // Returns all context keys that have stats stored.
  std::vector<ContextKey> context_keys() const {
    std::vector<ContextKey> keys;
    for (auto it = stats_by_context_.GetIterator(); it; ++it) {
      keys.push_back(it.key());
    }
    return keys;
  }

  // Returns the stats map for a given context key, or nullptr if not found.
  const StatsMap* FindStatsForContext(const ContextKey& key) const {
    return stats_by_context_.Find(key);
  }

 private:
  ContextKey GetContextKey(size_t key,
                           std::optional<MachineId> machine_id,
                           std::optional<TraceId> trace_id) const;

  StatsMap& GetOrCreateStatsMap(const ContextKey& ctx);

  base::FlatHashMap<ContextKey, StatsMap> stats_by_context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_GLOBAL_STATS_TRACKER_H_
