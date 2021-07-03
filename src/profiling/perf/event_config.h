/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_PROFILING_PERF_EVENT_CONFIG_H_
#define SRC_PROFILING_PERF_EVENT_CONFIG_H_

#include <cinttypes>
#include <functional>
#include <string>
#include <vector>

#include <linux/perf_event.h>
#include <stdint.h>
#include <sys/types.h>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/common/perf_events.gen.h"

namespace perfetto {
namespace protos {
namespace gen {
class PerfEventConfig;
}  // namespace gen
}  // namespace protos

namespace profiling {

// Parsed allow/deny-list for filtering samples.
// An empty filter set means that all targets are allowed.
struct TargetFilter {
  base::FlatSet<std::string> cmdlines;
  base::FlatSet<std::string> exclude_cmdlines;
  base::FlatSet<pid_t> pids;
  base::FlatSet<pid_t> exclude_pids;
  uint32_t additional_cmdline_count = 0;
};

// Describes a perf event for two purposes:
// * encoding the event in the perf_event_open syscall
// * echoing the counter's config in the trace packet defaults, so that the
//   parser can tell which datastream belongs to which counter.
// Note: It's slightly odd to decode & pass around values we don't use outside
// of reencoding back into a defaults proto. One option would be to carry the
// Timebase proto, but this won't fit with the eventual support of multiple
// counters, as at the proto level it'll be a distinct message from Timebase.
struct PerfCounter {
  enum class Type { kBuiltinCounter, kTracepoint, kRawEvent };

  Type type = Type::kBuiltinCounter;

  // Optional config-supplied name for the counter, to identify it during
  // trace parsing, does not affect the syscall.
  std::string name;

  // valid if kBuiltinCounter
  protos::gen::PerfEvents::Counter counter =
      protos::gen::PerfEvents::PerfEvents::UNKNOWN_COUNTER;
  // valid if kTracepoint. Example: "sched:sched_switch".
  std::string tracepoint_name;
  // valid if kTracepoint
  std::string tracepoint_filter;

  // sycall-level description of the event (perf_event_attr):
  uint32_t attr_type = 0;
  uint64_t attr_config = 0;
  uint64_t attr_config1 = 0;  // optional extension
  uint64_t attr_config2 = 0;  // optional extension

  Type event_type() const { return type; }

  static PerfCounter BuiltinCounter(std::string name,
                                    protos::gen::PerfEvents::Counter counter,
                                    uint32_t type,
                                    uint64_t config);

  static PerfCounter Tracepoint(std::string name,
                                std::string tracepoint_name,
                                std::string tracepoint_filter,
                                uint64_t id);

  static PerfCounter RawEvent(std::string name,
                              uint32_t type,
                              uint64_t config,
                              uint64_t config1,
                              uint64_t config2);
};

// Describes a single profiling configuration. Bridges the gap between the data
// source config proto, and the raw "perf_event_attr" structs to pass to the
// perf_event_open syscall.
class EventConfig {
 public:
  using tracepoint_id_fn_t =
      std::function<uint32_t(const std::string&, const std::string&)>;

  static base::Optional<EventConfig> Create(
      const DataSourceConfig& ds_config,
      tracepoint_id_fn_t tracepoint_id_lookup =
          [](const std::string&, const std::string&) { return 0; });

  static base::Optional<EventConfig> Create(
      const protos::gen::PerfEventConfig& pb_config,
      const DataSourceConfig& raw_ds_config,
      tracepoint_id_fn_t tracepoint_id_lookup);

  uint32_t ring_buffer_pages() const { return ring_buffer_pages_; }
  uint32_t read_tick_period_ms() const { return read_tick_period_ms_; }
  uint64_t samples_per_tick_limit() const { return samples_per_tick_limit_; }
  uint32_t remote_descriptor_timeout_ms() const {
    return remote_descriptor_timeout_ms_;
  }
  uint32_t unwind_state_clear_period_ms() const {
    return unwind_state_clear_period_ms_;
  }
  uint64_t max_enqueued_footprint_bytes() const {
    return max_enqueued_footprint_bytes_;
  }
  bool sample_callstacks() const { return sample_callstacks_; }
  const TargetFilter& filter() const { return target_filter_; }
  bool kernel_frames() const { return kernel_frames_; }
  perf_event_attr* perf_attr() const {
    return const_cast<perf_event_attr*>(&perf_event_attr_);
  }
  const PerfCounter& timebase_event() const { return timebase_event_; }
  const std::vector<std::string>& target_installed_by() const {
    return target_installed_by_;
  }
  const DataSourceConfig& raw_ds_config() const { return raw_ds_config_; }

 private:
  EventConfig(const DataSourceConfig& raw_ds_config,
              const perf_event_attr& pe,
              const PerfCounter& timebase_event,
              bool sample_callstacks,
              TargetFilter target_filter,
              bool kernel_frames,
              uint32_t ring_buffer_pages,
              uint32_t read_tick_period_ms,
              uint64_t samples_per_tick_limit,
              uint32_t remote_descriptor_timeout_ms,
              uint32_t unwind_state_clear_period_ms,
              uint64_t max_enqueued_footprint_bytes,
              std::vector<std::string> target_installed_by);

  // Parameter struct for the leader (timebase) perf_event_open syscall.
  perf_event_attr perf_event_attr_ = {};

  // Leader event, which is already described by |perf_event_attr_|. But this
  // additionally carries a tracepoint filter if that needs to be set via an
  // ioctl after creating the event.
  const PerfCounter timebase_event_;

  // TODO(rsavitski): consider adding an Optional<CallstackSampling> that
  // contains the kernel_frames_ and target_filter, once the complexity warrants
  // it.
  const bool sample_callstacks_;

  // Parsed allow/deny-list for filtering samples.
  const TargetFilter target_filter_;

  // If true, include kernel frames in the callstacks.
  const bool kernel_frames_;

  // Size (in 4k pages) of each per-cpu ring buffer shared with the kernel.
  // Must be a power of two.
  const uint32_t ring_buffer_pages_;

  // How often the ring buffers should be read.
  const uint32_t read_tick_period_ms_;

  // Guardrail for the amount of samples a given read attempt will extract from
  // *each* per-cpu buffer.
  const uint64_t samples_per_tick_limit_;

  // Timeout for proc-fd lookup.
  const uint32_t remote_descriptor_timeout_ms_;

  // Optional period for clearing cached unwinder state. Skipped if zero.
  const uint32_t unwind_state_clear_period_ms_;

  const uint64_t max_enqueued_footprint_bytes_;

  // Only profile target if it was installed by one of the packages given.
  // Special values are:
  // * "@system": installed on the system partition
  // * "@product": installed on the product partition
  // * "@null": sideloaded
  const std::vector<std::string> target_installed_by_;

  // The raw data source config, as a pbzero-generated C++ class.
  const DataSourceConfig raw_ds_config_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_EVENT_CONFIG_H_
