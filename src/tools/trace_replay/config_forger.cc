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

#include "src/tools/trace_replay/config_forger.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "protos/perfetto/config/data_source_config.gen.h"

namespace perfetto {
namespace trace_replay {

protos::gen::TraceConfig ForgeReplayConfig(
    const protos::gen::TraceConfig& original,
    const std::set<uint32_t>& used_buffers,
    uint64_t max_rel_ts_ns,
    const ForgeOptions& fopts) {
  // Build the forged config from scratch: the cppgen `.gen.h` plugin only
  // emits `clear_<field>()` for repeated fields, so we can't "deep-copy then
  // drop" optionals. Instead we copy only the fields we want to preserve.
  protos::gen::TraceConfig cfg;

  // Buffers are copied verbatim — size, fill policy, etc. stay the same.
  // Optionally force every buffer to use TRACE_BUFFER_V2.
  for (const auto& buf : original.buffers()) {
    auto* nb = cfg.add_buffers();
    *nb = buf;
    if (fopts.use_trace_buffer_v2) {
      nb->set_experimental_mode(
          protos::gen::TraceConfig_BufferConfig::TRACE_BUFFER_V2);
    }
  }

  // Pass-through pacing knobs and write-into-file behaviour so the replay
  // session pressures traced the same way the original did.
  if (original.has_flush_period_ms())
    cfg.set_flush_period_ms(original.flush_period_ms());
  if (original.has_data_source_stop_timeout_ms())
    cfg.set_data_source_stop_timeout_ms(original.data_source_stop_timeout_ms());
  if (original.has_write_into_file())
    cfg.set_write_into_file(original.write_into_file());
  if (original.has_file_write_period_ms())
    cfg.set_file_write_period_ms(original.file_write_period_ms());
  if (original.has_max_file_size_bytes())
    cfg.set_max_file_size_bytes(original.max_file_size_bytes());
  if (original.has_compression_type())
    cfg.set_compression_type(original.compression_type());
  if (original.has_notify_traceur())
    cfg.set_notify_traceur(original.notify_traceur());
  if (original.has_allow_user_build_tracing())
    cfg.set_allow_user_build_tracing(original.allow_user_build_tracing());
  if (original.has_prefer_suspend_clock_for_duration())
    cfg.set_prefer_suspend_clock_for_duration(
        original.prefer_suspend_clock_for_duration());
  if (original.has_incremental_state_config())
    *cfg.mutable_incremental_state_config() =
        original.incremental_state_config();

  // duration_ms policy.
  uint32_t duration_ms = 0;
  if (original.has_trigger_config() &&
      original.trigger_config().trigger_timeout_ms() > 0) {
    duration_ms = original.trigger_config().trigger_timeout_ms();
  } else if (original.has_duration_ms() && original.duration_ms() > 0) {
    duration_ms = original.duration_ms();
  } else {
    uint64_t safety = max_rel_ts_ns / 1000000ull + 5000ull;
    if (safety > std::numeric_limits<uint32_t>::max())
      safety = std::numeric_limits<uint32_t>::max();
    duration_ms = static_cast<uint32_t>(safety);
  }
  cfg.set_duration_ms(duration_ms);

  // Inject one data source per used buffer.
  std::vector<uint32_t> sorted(used_buffers.begin(), used_buffers.end());
  std::sort(sorted.begin(), sorted.end());
  for (uint32_t buf : sorted) {
    auto* ds = cfg.add_data_sources();
    auto* dc = ds->mutable_config();
    dc->set_name("replay.buf" + std::to_string(buf));
    dc->set_target_buffer(buf);
  }

  return cfg;
}

}  // namespace trace_replay
}  // namespace perfetto
