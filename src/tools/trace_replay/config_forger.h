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

#ifndef SRC_TOOLS_TRACE_REPLAY_CONFIG_FORGER_H_
#define SRC_TOOLS_TRACE_REPLAY_CONFIG_FORGER_H_

#include <cstdint>
#include <set>

#include "protos/perfetto/config/trace_config.gen.h"

namespace perfetto {
namespace trace_replay {

// Builds a replay TraceConfig from the original config:
//  - Keeps buffers verbatim.
//  - Drops data_sources, trigger_config, statsd_*, android_report_config,
//    enable_extra_guardrails.
//  - Adds one data source per buffer index in `used_buffers`, named
//    "replay.buf<N>", with target_buffer = N.
//  - duration_ms: copied from original.trigger_config.trigger_timeout_ms if
//    present, else original.duration_ms if >0, else
//    max_rel_ts_ns/1e6 + 5000 (safety cap).
struct ForgeOptions {
  bool use_trace_buffer_v2 = false;
};

protos::gen::TraceConfig ForgeReplayConfig(
    const protos::gen::TraceConfig& original,
    const std::set<uint32_t>& used_buffers,
    uint64_t max_rel_ts_ns,
    const ForgeOptions& fopts = {});

}  // namespace trace_replay
}  // namespace perfetto

#endif  // SRC_TOOLS_TRACE_REPLAY_CONFIG_FORGER_H_
