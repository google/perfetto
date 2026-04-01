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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_STATS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_STATS_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "perfetto/base/time.h"
#include "src/trace_processor/tables/metadata_tables_py.h"

namespace perfetto::trace_processor {

using MachineId = tables::MachineTable::Id;
using TraceId = tables::TraceFileTable::Id;

class TraceProcessorContext;

// Tracks stats for a specific (machine, trace) context.
// Delegates to GlobalStatsTracker, automatically passing the context's
// machine_id and trace_id.
class StatsTracker {
 public:
  explicit StatsTracker(TraceProcessorContext* context);

  void SetStats(size_t key, int64_t value);
  void IncrementStats(size_t key, int64_t increment = 1);
  void SetIndexedStats(size_t key, int index, int64_t value);
  void IncrementIndexedStats(size_t key, int index, int64_t increment = 1);
  int64_t GetStats(size_t key);
  std::optional<int64_t> GetIndexedStats(size_t key, int index);

  class ScopedStatsTracer {
   public:
    ScopedStatsTracer(StatsTracker* tracker, size_t key)
        : tracker_(tracker), key_(key), start_ns_(base::GetWallTimeNs()) {}

    ~ScopedStatsTracer() {
      if (!tracker_)
        return;
      auto delta_ns = base::GetWallTimeNs() - start_ns_;
      tracker_->IncrementStats(key_, delta_ns.count());
    }

    ScopedStatsTracer(ScopedStatsTracer&& other) noexcept { MoveImpl(&other); }

    ScopedStatsTracer& operator=(ScopedStatsTracer&& other) noexcept {
      MoveImpl(&other);
      return *this;
    }

   private:
    ScopedStatsTracer(const ScopedStatsTracer&) = delete;
    ScopedStatsTracer& operator=(const ScopedStatsTracer&) = delete;

    void MoveImpl(ScopedStatsTracer* other) {
      tracker_ = other->tracker_;
      key_ = other->key_;
      start_ns_ = other->start_ns_;
      other->tracker_ = nullptr;
    }

    StatsTracker* tracker_;
    size_t key_;
    base::TimeNanos start_ns_;
  };

  ScopedStatsTracer TraceExecutionTimeIntoStats(size_t key) {
    return ScopedStatsTracer(this, key);
  }

 private:
  std::optional<MachineId> machine_id() const;
  std::optional<TraceId> trace_id() const;

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_STATS_TRACKER_H_
