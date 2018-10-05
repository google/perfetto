/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SCHED_TRACKER_H_
#define SRC_TRACE_PROCESSOR_SCHED_TRACKER_H_

#include <array>
#include <limits>

#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// This class takes sched events from the trace and processes them to store
// as sched slices.
class SchedTracker {
 public:
  explicit SchedTracker(TraceProcessorContext*);
  SchedTracker(const SchedTracker&) = delete;
  SchedTracker& operator=(const SchedTracker&) = delete;
  virtual ~SchedTracker();

  StringId GetThreadNameId(uint32_t tid, base::StringView comm);

  // This method is called when a sched switch event is seen in the trace.
  virtual void PushSchedSwitch(uint32_t cpu,
                               uint64_t timestamp,
                               uint32_t prev_pid,
                               uint32_t prev_state,
                               uint32_t next_pid,
                               base::StringView next_comm);

  // This method is called when a cpu freq event is seen in the trace.
  // TODO(taylori): Move to a more appropriate class or rename class.
  virtual void PushCounter(uint64_t timestamp,
                           double value,
                           StringId name_id,
                           uint64_t ref,
                           RefType ref_type);

 private:
  // Used as the key in |prev_counters_| to find the previous counter with the
  // same ref and name_id.
  struct CounterKey {
    uint64_t ref;      // cpu, utid, ...
    StringId name_id;  // "cpufreq"

    bool operator==(const CounterKey& other) const {
      return (ref == other.ref && name_id == other.name_id);
    }

    struct Hasher {
      size_t operator()(const CounterKey& c) const {
        size_t const h1(std::hash<uint64_t>{}(c.ref));
        size_t const h2(std::hash<size_t>{}(c.name_id));
        return h1 ^ (h2 << 1);
      }
    };
  };

  // Represents a slice which is currently pending.
  struct PendingSchedSlice {
    size_t storage_index = std::numeric_limits<size_t>::max();
    uint32_t pid = 0;
  };

  // Store pending sched slices for each CPU.
  std::array<PendingSchedSlice, base::kMaxCpus> pending_sched_per_cpu_{};

  // Store pending counters for each counter key.
  std::unordered_map<CounterKey, size_t, CounterKey::Hasher>
      pending_counters_per_key_;

  // Timestamp of the previous event. Used to discard events arriving out
  // of order.
  uint64_t prev_timestamp_ = 0;

  StringId const idle_string_id_;

  TraceProcessorContext* const context_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_TRACKER_H_
