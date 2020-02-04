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

namespace perfetto {
namespace profiling {

// Describes a single profiling configuration. Bridges the gap between the data
// source config proto, and the raw "perf_event_attr" structs to pass to the
// perf_event_open syscall.
// TODO(rsavitski): instead of allowing arbitrary sampling flags, nail down a
// specific set, and simplify parsing at the same time?
// Also, for non-sample events (if they're possible), union of structs is
// interesting.
class EventConfig {
 public:
  static base::Optional<EventConfig> Create(const DataSourceConfig& ds_config) {
    protos::pbzero::PerfEventConfig::Decoder pb_config(
        ds_config.perf_event_config_raw());

    return EventConfig(pb_config);
  }

  uint32_t target_cpu() const { return target_cpu_; }

  perf_event_attr* perf_attr() const {
    return const_cast<perf_event_attr*>(&perf_event_attr_);
  }

 private:
  EventConfig(const protos::pbzero::PerfEventConfig::Decoder&) {
    auto& pe = perf_event_attr_;
    pe.size = sizeof(perf_event_attr);

    pe.exclude_kernel = true;
    pe.disabled = false;

    // Ask the kernel to tune sampling period to get ~100 Hz.
    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    pe.sample_freq = 100;
    pe.freq = true;

    pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                     PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;
    // Needs to be < ((u16)(~0u)), and have bottom 8 bits clear.
    pe.sample_stack_user = (1u << 15);
    pe.sample_regs_user =
        PerfUserRegsMaskForArch(unwindstack::Regs::CurrentArch());

    // for PERF_SAMPLE_TIME
    pe.clockid = CLOCK_BOOTTIME;
    pe.use_clockid = true;
  }

  // TODO(rsavitski): for now hardcode each session to be for a single cpu's
  // scope. In general a config will correspond to N cpus and/or tids.
  uint32_t target_cpu_ = 0;

  // TODO(rsavitski): if we allow for event groups containing multiple sampled
  // counters, we'll need to vary the .type & .config fields per
  // perf_event_open.
  perf_event_attr perf_event_attr_ = {};
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_EVENT_CONFIG_H_
