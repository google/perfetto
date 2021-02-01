/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/perf/event_config.h"

#include <linux/perf_event.h>
#include <time.h>

#include <unwindstack/Regs.h>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/profiling/normalize.h"
#include "src/profiling/perf/regs_parsing.h"

#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"

namespace perfetto {
namespace profiling {

namespace {
constexpr uint64_t kDefaultSamplingFrequencyHz = 10;
constexpr uint32_t kDefaultDataPagesPerRingBuffer = 256;  // 1 MB: 256x 4k pages
constexpr uint32_t kDefaultReadTickPeriodMs = 100;
constexpr uint32_t kDefaultRemoteDescriptorTimeoutMs = 100;

base::Optional<std::string> Normalize(const protozero::ConstChars& src) {
  // Construct a null-terminated string that will be mutated by the normalizer.
  std::vector<char> base(src.size + 1);
  memcpy(base.data(), src.data, src.size);
  base[src.size] = '\0';

  char* new_start = base.data();
  ssize_t new_sz = NormalizeCmdLine(&new_start, base.size());
  if (new_sz < 0) {
    PERFETTO_ELOG("Failed to normalize config cmdline [%s], aborting",
                  base.data());
    return base::nullopt;
  }
  return base::make_optional<std::string>(new_start,
                                          static_cast<size_t>(new_sz));
}

// Acceptable forms: "sched/sched_switch" or "sched:sched_switch".
std::pair<std::string, std::string> SplitTracepointString(
    const std::string& input) {
  auto slash_pos = input.find("/");
  if (slash_pos != std::string::npos)
    return std::make_pair(input.substr(0, slash_pos),
                          input.substr(slash_pos + 1));

  auto colon_pos = input.find(":");
  if (colon_pos != std::string::npos)
    return std::make_pair(input.substr(0, colon_pos),
                          input.substr(colon_pos + 1));

  return std::make_pair("", input);
}

// If set, the returned id is guaranteed to be non-zero.
base::Optional<uint32_t> ParseTracepointAndResolveId(
    const protos::pbzero::TracepointEventConfig::Decoder& tracepoint,
    EventConfig::tracepoint_id_fn_t tracepoint_id_lookup) {
  std::string full_name = tracepoint.name().ToStdString();
  std::string tp_group;
  std::string tp_name;
  std::tie(tp_group, tp_name) = SplitTracepointString(full_name);
  if (tp_group.empty() || tp_name.empty()) {
    PERFETTO_ELOG(
        "Invalid tracepoint format: %s. Should be a full path like "
        "sched:sched_switch or sched/sched_switch.",
        full_name.c_str());
    return base::nullopt;
  }

  uint32_t tracepoint_id = tracepoint_id_lookup(tp_group, tp_name);
  if (!tracepoint_id) {
    PERFETTO_ELOG(
        "Failed to resolve tracepoint %s to its id. Check that tracefs is "
        "accessible and the event exists.",
        full_name.c_str());
    return base::nullopt;
  }
  return base::make_optional(tracepoint_id);
}

// returns |base::nullopt| if any of the input cmdlines couldn't be normalized.
base::Optional<TargetFilter> ParseTargetFilter(
    const protos::pbzero::PerfEventConfig::Decoder& cfg) {
  TargetFilter filter;
  for (auto it = cfg.target_cmdline(); it; ++it) {
    base::Optional<std::string> opt = Normalize(*it);
    if (!opt.has_value())
      return base::nullopt;
    filter.cmdlines.insert(std::move(opt.value()));
  }

  for (auto it = cfg.exclude_cmdline(); it; ++it) {
    base::Optional<std::string> opt = Normalize(*it);
    if (!opt.has_value())
      return base::nullopt;
    filter.exclude_cmdlines.insert(std::move(opt.value()));
  }

  for (auto it = cfg.target_pid(); it; ++it) {
    filter.pids.insert(*it);
  }

  for (auto it = cfg.exclude_pid(); it; ++it) {
    filter.exclude_pids.insert(*it);
  }

  filter.additional_cmdline_count = cfg.additional_cmdline_count();
  return base::make_optional(std::move(filter));
}

constexpr bool IsPowerOfTwo(size_t v) {
  return (v != 0 && ((v & (v - 1)) == 0));
}

// returns |base::nullopt| if the input is invalid.
base::Optional<uint32_t> ChooseActualRingBufferPages(uint32_t config_value) {
  if (!config_value) {
    static_assert(IsPowerOfTwo(kDefaultDataPagesPerRingBuffer), "");
    return base::make_optional(kDefaultDataPagesPerRingBuffer);
  }

  if (!IsPowerOfTwo(config_value)) {
    PERFETTO_ELOG("kernel buffer size must be a power of two pages");
    return base::nullopt;
  }

  return base::make_optional(config_value);
}

}  // namespace

// static
base::Optional<EventConfig> EventConfig::Create(
    const DataSourceConfig& ds_config,
    tracepoint_id_fn_t tracepoint_id_lookup) {
  protos::pbzero::PerfEventConfig::Decoder event_config_pb(
      ds_config.perf_event_config_raw());
  return EventConfig::Create(event_config_pb, ds_config, tracepoint_id_lookup);
}

// static
base::Optional<EventConfig> EventConfig::Create(
    const protos::pbzero::PerfEventConfig::Decoder& pb_config,
    const DataSourceConfig& raw_ds_config,
    tracepoint_id_fn_t tracepoint_id_lookup) {
  // The counter (aka event or timebase) is either a tracepoint, or the
  // implicit default - CPU timer.
  uint32_t tracepoint_id = 0;
  if (pb_config.has_tracepoint()) {
    base::Optional<uint32_t> maybe_id = ParseTracepointAndResolveId(
        protos::pbzero::TracepointEventConfig::Decoder(pb_config.tracepoint()),
        tracepoint_id_lookup);
    if (!maybe_id)
      return base::nullopt;
    tracepoint_id = *maybe_id;
  }

  base::Optional<TargetFilter> filter = ParseTargetFilter(pb_config);
  if (!filter.has_value())
    return base::nullopt;

  base::Optional<uint32_t> ring_buffer_pages =
      ChooseActualRingBufferPages(pb_config.ring_buffer_pages());
  if (!ring_buffer_pages.has_value())
    return base::nullopt;

  uint32_t remote_descriptor_timeout_ms =
      pb_config.remote_descriptor_timeout_ms()
          ? pb_config.remote_descriptor_timeout_ms()
          : kDefaultRemoteDescriptorTimeoutMs;

  uint32_t read_tick_period_ms = pb_config.ring_buffer_read_period_ms()
                                     ? pb_config.ring_buffer_read_period_ms()
                                     : kDefaultReadTickPeriodMs;

  uint32_t sampling_frequency = pb_config.sampling_frequency()
                                    ? pb_config.sampling_frequency()
                                    : kDefaultSamplingFrequencyHz;

  // Take the ratio of sampling and reading frequencies, which gives the
  // upper bound on number of samples per tick (for a single per-cpu buffer).
  // Overflow not a concern for sane inputs.
  uint32_t expected_samples_per_tick =
      1 + (sampling_frequency * read_tick_period_ms) / 1000;

  // Use double the expected value as the actual guardrail (don't assume that
  // periodic read task is as exact as the kernel).
  uint32_t samples_per_tick_limit = 2 * expected_samples_per_tick;
  PERFETTO_DCHECK(samples_per_tick_limit > 0);
  PERFETTO_DLOG("Capping samples (not records) per tick to [%" PRIu32 "]",
                samples_per_tick_limit);

  // Build the underlying syscall config struct.
  perf_event_attr pe = {};
  pe.size = sizeof(perf_event_attr);

  pe.disabled = false;

  // Event being counted (timebase).
  if (tracepoint_id) {
    pe.type = PERF_TYPE_TRACEPOINT;
    pe.config = tracepoint_id;
  } else {
    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_CPU_CLOCK;
  }

  // Ask the kernel to sample at a given frequency.
  pe.freq = true;
  pe.sample_freq = sampling_frequency;

  pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_STACK_USER |
                   PERF_SAMPLE_REGS_USER;
  // PERF_SAMPLE_TIME:
  // We used to use CLOCK_BOOTTIME, but that is not nmi-safe, and therefore
  // works only for software events.
  pe.clockid = CLOCK_MONOTONIC_RAW;
  pe.use_clockid = true;
  // PERF_SAMPLE_STACK_USER:
  // Needs to be < ((u16)(~0u)), and have bottom 8 bits clear.
  // Note that the kernel still needs to make space for the other parts of the
  // sample (up to the max record size of 64k), so the effective maximum
  // can be lower than this.
  pe.sample_stack_user = (1u << 16) - 256;
  // PERF_SAMPLE_REGS_USER:
  pe.sample_regs_user =
      PerfUserRegsMaskForArch(unwindstack::Regs::CurrentArch());

  // Optional kernel callchains:
  if (pb_config.kernel_frames()) {
    pe.sample_type |= PERF_SAMPLE_CALLCHAIN;
    pe.exclude_callchain_user = true;
  }

  return EventConfig(pb_config, raw_ds_config, pe, ring_buffer_pages.value(),
                     read_tick_period_ms, samples_per_tick_limit,
                     remote_descriptor_timeout_ms, std::move(filter.value()));
}

EventConfig::EventConfig(const protos::pbzero::PerfEventConfig::Decoder& cfg,
                         const DataSourceConfig& raw_ds_config,
                         const perf_event_attr& pe,
                         uint32_t ring_buffer_pages,
                         uint32_t read_tick_period_ms,
                         uint32_t samples_per_tick_limit,
                         uint32_t remote_descriptor_timeout_ms,
                         TargetFilter target_filter)
    : target_all_cpus_(cfg.all_cpus()),
      ring_buffer_pages_(ring_buffer_pages),
      perf_event_attr_(pe),
      read_tick_period_ms_(read_tick_period_ms),
      samples_per_tick_limit_(samples_per_tick_limit),
      target_filter_(std::move(target_filter)),
      remote_descriptor_timeout_ms_(remote_descriptor_timeout_ms),
      unwind_state_clear_period_ms_(cfg.unwind_state_clear_period_ms()),
      kernel_frames_(cfg.kernel_frames()),
      raw_ds_config_(raw_ds_config) /* copy */ {}

}  // namespace profiling
}  // namespace perfetto
