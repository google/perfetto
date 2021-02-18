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
#include "perfetto/ext/base/utils.h"
#include "perfetto/profiling/normalize.h"
#include "src/profiling/perf/regs_parsing.h"

#include "protos/perfetto/config/profiling/perf_event_config.gen.h"

namespace perfetto {
namespace profiling {

namespace {
constexpr uint64_t kDefaultSamplingFrequencyHz = 10;
constexpr uint32_t kDefaultDataPagesPerRingBuffer = 256;  // 1 MB: 256x 4k pages
constexpr uint32_t kDefaultReadTickPeriodMs = 100;
constexpr uint32_t kDefaultRemoteDescriptorTimeoutMs = 100;

base::Optional<std::string> Normalize(const std::string& src) {
  // Construct a null-terminated string that will be mutated by the normalizer.
  std::vector<char> base(src.size() + 1);
  memcpy(base.data(), src.data(), src.size());
  base[src.size()] = '\0';

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
    const protos::gen::PerfEventConfig::Tracepoint& tracepoint,
    EventConfig::tracepoint_id_fn_t tracepoint_id_lookup) {
  std::string full_name = tracepoint.name();
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

// Returns |base::nullopt| if any of the input cmdlines couldn't be normalized.
// |T| is either gen::PerfEventConfig or gen::PerfEventConfig::Scope.
template <typename T>
base::Optional<TargetFilter> ParseTargetFilter(const T& cfg) {
  TargetFilter filter;
  for (const auto& str : cfg.target_cmdline()) {
    base::Optional<std::string> opt = Normalize(str);
    if (!opt.has_value()) {
      PERFETTO_ELOG("Failure normalizing cmdline: [%s]", str.c_str());
      return base::nullopt;
    }
    filter.cmdlines.insert(std::move(opt.value()));
  }

  for (const auto& str : cfg.exclude_cmdline()) {
    base::Optional<std::string> opt = Normalize(str);
    if (!opt.has_value()) {
      PERFETTO_ELOG("Failure normalizing cmdline: [%s]", str.c_str());
      return base::nullopt;
    }
    filter.exclude_cmdlines.insert(std::move(opt.value()));
  }

  for (const int32_t pid : cfg.target_pid()) {
    filter.pids.insert(pid);
  }

  for (const int32_t pid : cfg.exclude_pid()) {
    filter.exclude_pids.insert(pid);
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

base::Optional<PerfCounter> ToPerfCounter(
    protos::gen::PerfEventConfig::Counter pb_enum) {
  using protos::gen::PerfEventConfig;
  switch (static_cast<int>(pb_enum)) {  // cast to pacify -Wswitch-enum
    case PerfEventConfig::SW_CPU_CLOCK:
      return base::make_optional<PerfCounter>(PERF_TYPE_SOFTWARE,
                                              PERF_COUNT_SW_CPU_CLOCK);
    case PerfEventConfig::SW_PAGE_FAULTS:
      return base::make_optional<PerfCounter>(PERF_TYPE_SOFTWARE,
                                              PERF_COUNT_SW_PAGE_FAULTS);
    case PerfEventConfig::HW_CPU_CYCLES:
      return base::make_optional<PerfCounter>(PERF_TYPE_HARDWARE,
                                              PERF_COUNT_HW_CPU_CYCLES);
    case PerfEventConfig::HW_INSTRUCTIONS:
      return base::make_optional<PerfCounter>(PERF_TYPE_HARDWARE,
                                              PERF_COUNT_HW_INSTRUCTIONS);
    default:
      PERFETTO_ELOG("Unrecognised PerfEventConfig::Counter enum value: %zu",
                    static_cast<size_t>(pb_enum));
      return base::nullopt;
  }
}

}  // namespace

// static
base::Optional<EventConfig> EventConfig::Create(
    const DataSourceConfig& ds_config,
    tracepoint_id_fn_t tracepoint_id_lookup) {
  protos::gen::PerfEventConfig pb_config;
  if (!pb_config.ParseFromString(ds_config.perf_event_config_raw()))
    return base::nullopt;

  return EventConfig::Create(pb_config, ds_config, tracepoint_id_lookup);
}

// static
base::Optional<EventConfig> EventConfig::Create(
    const protos::gen::PerfEventConfig& pb_config,
    const DataSourceConfig& raw_ds_config,
    tracepoint_id_fn_t tracepoint_id_lookup) {
  // Timebase: sampling interval.
  uint64_t sampling_frequency = 0;
  uint64_t sampling_period = 0;
  if (pb_config.timebase().period()) {
    sampling_period = pb_config.timebase().period();
  } else if (pb_config.timebase().frequency()) {
    sampling_frequency = pb_config.timebase().frequency();
  } else if (pb_config.sampling_frequency()) {  // backwards compatibility
    sampling_frequency = pb_config.sampling_frequency();
  } else {
    sampling_frequency = kDefaultSamplingFrequencyHz;
  }
  PERFETTO_DCHECK(sampling_period && !sampling_frequency ||
                  !sampling_period && sampling_frequency);

  // Timebase event. Default: CPU timer.
  PerfCounter timebase_event;
  if (pb_config.timebase().has_counter()) {
    auto maybe_counter = ToPerfCounter(pb_config.timebase().counter());
    if (!maybe_counter)
      return base::nullopt;
    timebase_event = *maybe_counter;

  } else if (pb_config.timebase().has_tracepoint() ||
             pb_config.has_tracepoint()) {
    const auto& tracepoint_pb =
        pb_config.timebase().has_tracepoint()
            ? pb_config.timebase().tracepoint()
            : pb_config.tracepoint();  // backwards compatibility
    base::Optional<uint32_t> maybe_id =
        ParseTracepointAndResolveId(tracepoint_pb, tracepoint_id_lookup);
    if (!maybe_id)
      return base::nullopt;
    timebase_event =
        PerfCounter{PERF_TYPE_TRACEPOINT, *maybe_id, tracepoint_pb.filter()};

  } else {
    timebase_event = PerfCounter{PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK};
  }

  // Callstack sampling.
  bool sample_callstacks = false;
  bool kernel_frames = false;
  TargetFilter target_filter;
  bool legacy_config = pb_config.all_cpus();  // all_cpus was mandatory before
  if (pb_config.has_callstack_sampling() || legacy_config) {
    sample_callstacks = true;

    // Process scoping.
    auto maybe_filter =
        pb_config.callstack_sampling().has_scope()
            ? ParseTargetFilter(pb_config.callstack_sampling().scope())
            : ParseTargetFilter(pb_config);  // backwards compatibility
    if (!maybe_filter.has_value())
      return base::nullopt;

    target_filter = std::move(maybe_filter.value());

    // Inclusion of kernel callchains.
    kernel_frames = pb_config.callstack_sampling().kernel_frames() ||
                    pb_config.kernel_frames();
  }

  // Ring buffer options.
  base::Optional<uint32_t> ring_buffer_pages =
      ChooseActualRingBufferPages(pb_config.ring_buffer_pages());
  if (!ring_buffer_pages.has_value())
    return base::nullopt;

  uint32_t read_tick_period_ms = pb_config.ring_buffer_read_period_ms()
                                     ? pb_config.ring_buffer_read_period_ms()
                                     : kDefaultReadTickPeriodMs;

  // Calculate a rough upper limit for the amount of samples the producer
  // should read per read tick, as a safeguard against getting stuck chasing the
  // ring buffer head indefinitely.
  uint64_t samples_per_tick_limit = 0;
  if (sampling_frequency) {
    // expected = rate * period, with a conversion of period from ms to s:
    uint64_t expected_samples_per_tick =
        1 + (sampling_frequency * read_tick_period_ms) / 1000;
    // Double the the limit to account of actual sample rate uncertainties, as
    // well as any other factors:
    samples_per_tick_limit = 2 * expected_samples_per_tick;
  } else {  // sampling_period
    // We don't know the sample rate that a fixed period would cause, but we can
    // still estimate how many samples will fit in one pass of the ring buffer
    // (with the assumption that we don't want to read more than one buffer's
    // capacity within a tick).
    // TODO(rsavitski): for now, make an extremely conservative guess of an 8
    // byte sample (stack sampling samples can be up to 64KB). This is most
    // likely as good as no limit in practice.
    samples_per_tick_limit = *ring_buffer_pages * (base::kPageSize / 8);
  }
  PERFETTO_DLOG("Capping samples (not records) per tick to [%" PRIu64 "]",
                samples_per_tick_limit);
  if (samples_per_tick_limit == 0)
    return base::nullopt;

  // Android-specific options.
  uint32_t remote_descriptor_timeout_ms =
      pb_config.remote_descriptor_timeout_ms()
          ? pb_config.remote_descriptor_timeout_ms()
          : kDefaultRemoteDescriptorTimeoutMs;

  // Build the underlying syscall config struct.
  perf_event_attr pe = {};
  pe.size = sizeof(perf_event_attr);
  pe.disabled = true;  // will be activated via ioctl

  // Sampling timebase.
  pe.type = timebase_event.type;
  pe.config = timebase_event.config;
  if (sampling_frequency) {
    pe.freq = true;
    pe.sample_freq = sampling_frequency;
  } else {
    pe.sample_period = sampling_period;
  }

  // What the samples will contain.
  pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_READ;
  // PERF_SAMPLE_TIME:
  // We used to use CLOCK_BOOTTIME, but that is not nmi-safe, and therefore
  // works only for software events.
  pe.clockid = CLOCK_MONOTONIC_RAW;
  pe.use_clockid = true;

  if (sample_callstacks) {
    pe.sample_type |= PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER;
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
    if (kernel_frames) {
      pe.sample_type |= PERF_SAMPLE_CALLCHAIN;
      pe.exclude_callchain_user = true;
    }
  }

  return EventConfig(raw_ds_config, pe, timebase_event, sample_callstacks,
                     std::move(target_filter), kernel_frames,
                     ring_buffer_pages.value(), read_tick_period_ms,
                     samples_per_tick_limit, remote_descriptor_timeout_ms,
                     pb_config.unwind_state_clear_period_ms());
}

EventConfig::EventConfig(const DataSourceConfig& raw_ds_config,
                         const perf_event_attr& pe,
                         const PerfCounter& timebase_event,
                         bool sample_callstacks,
                         TargetFilter target_filter,
                         bool kernel_frames,
                         uint32_t ring_buffer_pages,
                         uint32_t read_tick_period_ms,
                         uint64_t samples_per_tick_limit,
                         uint32_t remote_descriptor_timeout_ms,
                         uint32_t unwind_state_clear_period_ms)
    : perf_event_attr_(pe),
      timebase_event_(timebase_event),
      sample_callstacks_(sample_callstacks),
      target_filter_(std::move(target_filter)),
      kernel_frames_(kernel_frames),
      ring_buffer_pages_(ring_buffer_pages),
      read_tick_period_ms_(read_tick_period_ms),
      samples_per_tick_limit_(samples_per_tick_limit),
      remote_descriptor_timeout_ms_(remote_descriptor_timeout_ms),
      unwind_state_clear_period_ms_(unwind_state_clear_period_ms),
      raw_ds_config_(raw_ds_config) /* full copy */ {}

}  // namespace profiling
}  // namespace perfetto
