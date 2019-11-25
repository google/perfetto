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

#include "perfetto/ext/base/optional.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"

namespace perfetto {
namespace profiling {

// Describes a single profiling configuration. Bridges the gap between the data
// source config proto, and the raw "perf_event_attr" structs to pass to the
// perf_event_open syscall.
// TODO(rsavitski): make sampling conditional? Or should we always go through
// the sampling interface for simplicity? Reads can be done on-demand even if
// sampling is on. So the question becomes whether we need *only* on-demand
// reads.
class EventConfig {
 public:
  static base::Optional<EventConfig> Create(const DataSourceConfig& ds_config) {
    protos::pbzero::PerfEventConfig::Decoder pb_config(
        ds_config.perf_event_config_raw());

    if (!pb_config.has_tid())
      return base::nullopt;

    return EventConfig(pb_config);
  }

  int32_t target_tid() const { return target_tid_; }

  perf_event_attr* perf_attr() const {
    return const_cast<perf_event_attr*>(&perf_event_attr_);
  }

 private:
  EventConfig(const protos::pbzero::PerfEventConfig::Decoder& pb_config)
      : target_tid_(pb_config.tid()) {
    auto& pe = perf_event_attr_;
    memset(&pe, 0, sizeof(perf_event_attr));
    pe.size = sizeof(perf_event_attr);

    pe.exclude_kernel = true;
    pe.disabled = false;

    // Ask the kernel to tune sampling period to get ~100 Hz.
    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
    pe.sample_freq = 100;
    pe.freq = true;

    pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_STACK_USER;
    // Needs to be < ((u16)(~0u)), and have bottom 8 bits clear.
    pe.sample_stack_user = (1u << 15);

    // Note: can't use inherit with task-scoped event mmap
    pe.inherit = false;
  }

  // TODO(rsavitski): this will have to represent entire event groups, thus this
  // class will represent N events. So we'll need N cpus/tids, but likely still
  // a single perf_event_attr.
  int32_t target_tid_ = 0;
  perf_event_attr perf_event_attr_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_EVENT_CONFIG_H_
