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

#include <linux/perf_event.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include <unwindstack/Regs.h>

#include "perfetto/ext/base/optional.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/profiling/perf/regs_parsing.h"

#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"

namespace {
constexpr uint64_t kDefaultSamplingFrequency = 100;  // Hz
}

namespace perfetto {
namespace profiling {

// Describes a single profiling configuration. Bridges the gap between the data
// source config proto, and the raw "perf_event_attr" structs to pass to the
// perf_event_open syscall.
class EventConfig {
 public:
  static base::Optional<EventConfig> Create(const DataSourceConfig& ds_config) {
    protos::pbzero::PerfEventConfig::Decoder pb_config(
        ds_config.perf_event_config_raw());

    return EventConfig(pb_config);
  }

  uint32_t target_all_cpus() const { return target_all_cpus_; }
  size_t ring_buffer_pages() const { return ring_buffer_pages_; }

  perf_event_attr* perf_attr() const {
    return const_cast<perf_event_attr*>(&perf_event_attr_);
  }

 private:
  EventConfig(const protos::pbzero::PerfEventConfig::Decoder& cfg)
      : target_all_cpus_(cfg.all_cpus()),
        ring_buffer_pages_(cfg.ring_buffer_pages()) {
    auto& pe = perf_event_attr_;
    pe.size = sizeof(perf_event_attr);

    pe.disabled = false;

    // Ask the kernel to sample at a given frequency.
    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    pe.freq = true;
    pe.sample_freq = (cfg.sampling_frequency() > 0) ? cfg.sampling_frequency()
                                                    : kDefaultSamplingFrequency;

    pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                     PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;
    // PERF_SAMPLE_TIME:
    pe.clockid = CLOCK_BOOTTIME;
    pe.use_clockid = true;
    // PERF_SAMPLE_STACK_USER:
    // Needs to be < ((u16)(~0u)), and have bottom 8 bits clear.
    pe.sample_stack_user = (1u << 15);
    // PERF_SAMPLE_REGS_USER:
    pe.sample_regs_user =
        PerfUserRegsMaskForArch(unwindstack::Regs::CurrentArch());
  }

  // If true, process all system-wide samples.
  const bool target_all_cpus_;

  // Size (in 4k pages) of each per-cpu ring buffer shared with the kernel. If
  // zero, |EventReader| will choose a default value. Must be a power of two
  // otherwise.
  const size_t ring_buffer_pages_;

  // TODO(rsavitski): if we allow for event groups containing multiple sampled
  // counters, we'll need to vary the .type & .config fields per
  // perf_event_open.
  perf_event_attr perf_event_attr_ = {};
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_EVENT_CONFIG_H_
