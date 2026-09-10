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

#include "src/trace_processor/importers/perf/perf_counter.h"

#include <cinttypes>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::perf_importer {

PerfCounter::PerfCounter(TraceProcessorContext* context,
                         tables::TrackTable::Id track_id,
                         bool is_timebase)
    : context_(context),
      counter_table_(*context->storage->mutable_counter_table()),
      track_id_(track_id),
      is_timebase_(is_timebase) {
  PERFETTO_DCHECK(context);
}

tables::CounterTable::Id PerfCounter::AddDelta(int64_t ts, double delta) {
  last_count_ += delta;
  return counter_table_.Insert({ts, track_id_, last_count_}).id;
}

tables::CounterTable::Id PerfCounter::AddCount(int64_t ts,
                                               double count,
                                               std::optional<uint32_t> cpu) {
  // On heterogeneous multi-core architectures (e.g. big.LITTLE), cluster PMU
  // readouts can experience baseline shifts across core migrations. Track
  // per-CPU baselines to correctly aggregate incremental deltas.
  if (cpu.has_value()) {
    double& last_cpu = last_count_per_cpu_[*cpu];
    if (PERFETTO_UNLIKELY(count < last_cpu)) {
      PERFETTO_DLOG("Non-monotonic perf counter value on track %" PRIu32
                    " (cpu %" PRIu32 "): %f < %f",
                    track_id_.value, *cpu, count, last_cpu);
      context_->stats_tracker->IncrementStats(
          stats::perf_counter_non_monotonic);
      count = last_cpu;
    }
    double delta = count - last_cpu;
    last_cpu = count;
    last_count_ += delta;
    return counter_table_.Insert({ts, track_id_, last_count_}).id;
  }

  if (PERFETTO_UNLIKELY(count < last_count_)) {
    PERFETTO_DLOG("Non-monotonic perf counter value on track %" PRIu32
                  ": %f < %f",
                  track_id_.value, count, last_count_);
    context_->stats_tracker->IncrementStats(stats::perf_counter_non_monotonic);
    // TODO(b/334978369): Consider detecting counter wraps/resets.
    count = last_count_;
  }
  last_count_ = count;
  return counter_table_.Insert({ts, track_id_, last_count_}).id;
}

}  // namespace perfetto::trace_processor::perf_importer
