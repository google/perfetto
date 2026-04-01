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

#include "src/trace_processor/importers/common/global_stats_tracker.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/storage/stats.h"

namespace perfetto::trace_processor {

GlobalStatsTracker::GlobalStatsTracker() = default;

void GlobalStatsTracker::SetStats(std::optional<MachineId> machine_id,
                                  std::optional<TraceId> trace_id,
                                  size_t key,
                                  int64_t value) {
  PERFETTO_DCHECK(key < stats::kNumKeys);
  PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
  auto ctx = GetContextKey(key, machine_id, trace_id);
  GetOrCreateStatsMap(ctx)[key].value = value;
}

void GlobalStatsTracker::IncrementStats(std::optional<MachineId> machine_id,
                                        std::optional<TraceId> trace_id,
                                        size_t key,
                                        int64_t increment) {
  PERFETTO_DCHECK(key < stats::kNumKeys);
  PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
  auto ctx = GetContextKey(key, machine_id, trace_id);
  GetOrCreateStatsMap(ctx)[key].value += increment;
}

void GlobalStatsTracker::SetIndexedStats(std::optional<MachineId> machine_id,
                                         std::optional<TraceId> trace_id,
                                         size_t key,
                                         int index,
                                         int64_t value) {
  PERFETTO_DCHECK(key < stats::kNumKeys);
  PERFETTO_DCHECK(stats::kTypes[key] == stats::kIndexed);
  auto ctx = GetContextKey(key, machine_id, trace_id);
  GetOrCreateStatsMap(ctx)[key].indexed_values[index] = value;
}

void GlobalStatsTracker::IncrementIndexedStats(
    std::optional<MachineId> machine_id,
    std::optional<TraceId> trace_id,
    size_t key,
    int index,
    int64_t increment) {
  PERFETTO_DCHECK(key < stats::kNumKeys);
  PERFETTO_DCHECK(stats::kTypes[key] == stats::kIndexed);
  auto ctx = GetContextKey(key, machine_id, trace_id);
  GetOrCreateStatsMap(ctx)[key].indexed_values[index] += increment;
}

int64_t GlobalStatsTracker::GetStats(std::optional<MachineId> machine_id,
                                     std::optional<TraceId> trace_id,
                                     size_t key) {
  PERFETTO_DCHECK(key < stats::kNumKeys);
  PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
  auto ctx = GetContextKey(key, machine_id, trace_id);
  auto* map = stats_by_context_.Find(ctx);
  if (!map) {
    return 0;
  }
  return (*map)[key].value;
}

std::optional<int64_t> GlobalStatsTracker::GetIndexedStats(
    std::optional<MachineId> machine_id,
    std::optional<TraceId> trace_id,
    size_t key,
    int index) {
  PERFETTO_DCHECK(key < stats::kNumKeys);
  PERFETTO_DCHECK(stats::kTypes[key] == stats::kIndexed);
  auto ctx = GetContextKey(key, machine_id, trace_id);
  auto* map = stats_by_context_.Find(ctx);
  if (!map) {
    return std::nullopt;
  }
  auto kv = (*map)[key].indexed_values.find(index);
  if (kv != (*map)[key].indexed_values.end()) {
    return kv->second;
  }
  return std::nullopt;
}

GlobalStatsTracker::ContextKey GlobalStatsTracker::GetContextKey(
    size_t key,
    std::optional<MachineId> machine_id,
    std::optional<TraceId> trace_id) const {
  switch (stats::kScopes[key]) {
    case stats::Scope::kGlobal:
      return {std::nullopt, std::nullopt};
    case stats::Scope::kMachine:
      PERFETTO_CHECK(machine_id.has_value());
      return {machine_id, std::nullopt};
    case stats::Scope::kTrace:
      PERFETTO_CHECK(trace_id.has_value());
      return {std::nullopt, trace_id};
    case stats::Scope::kMachineAndTrace:
      return {machine_id, trace_id};
    case stats::Scope::kNumScopes:
      PERFETTO_FATAL("Invalid scope");
  }
  PERFETTO_FATAL("For GCC");
}

GlobalStatsTracker::StatsMap& GlobalStatsTracker::GetOrCreateStatsMap(
    const ContextKey& ctx) {
  auto* existing = stats_by_context_.Find(ctx);
  if (existing) {
    return *existing;
  }
  stats_by_context_.Insert(ctx, StatsMap{});
  return *stats_by_context_.Find(ctx);
}

}  // namespace perfetto::trace_processor
