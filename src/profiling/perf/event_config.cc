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
#include <optional>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/utils.h"
#include "src/profiling/perf/regs_parsing.h"

#include "protos/perfetto/common/perf_events.gen.h"
#include "protos/perfetto/config/profiling/perf_event_config.gen.h"

namespace perfetto {
namespace profiling {

namespace {
constexpr uint64_t kDefaultSamplingFrequencyHz = 10;
constexpr uint32_t kDefaultDataPagesPerRingBuffer = 256;  // 1 MB: 256x 4k pages
constexpr uint32_t kDefaultReadTickPeriodMs = 100;
constexpr uint32_t kDefaultRemoteDescriptorTimeoutMs = 100;

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
std::optional<uint32_t> ParseTracepointAndResolveId(
    const protos::gen::PerfEvents::Tracepoint& tracepoint,
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
    return std::nullopt;
  }

  uint32_t tracepoint_id = tracepoint_id_lookup(tp_group, tp_name);
  if (!tracepoint_id) {
    PERFETTO_ELOG(
        "Failed to resolve tracepoint %s to its id. Check that tracefs is "
        "accessible and the event exists.",
        full_name.c_str());
    return std::nullopt;
  }
  return std::make_optional(tracepoint_id);
}

// |T| is either gen::PerfEventConfig or gen::PerfEventConfig::Scope.
// Note: the semantics of target_cmdline and exclude_cmdline were changed since
// their original introduction. They used to be put through a canonicalization
// function that simplified them to the binary name alone. We no longer do this,
// regardless of whether we're parsing an old-style config. The overall outcome
// shouldn't change for almost all existing uses.
template <typename T>
TargetFilter ParseTargetFilter(
    const T& cfg,
    std::optional<ProcessSharding> process_sharding) {
  TargetFilter filter;
  for (const auto& str : cfg.target_cmdline()) {
    filter.cmdlines.push_back(str);
  }
  for (const auto& str : cfg.exclude_cmdline()) {
    filter.exclude_cmdlines.push_back(str);
  }
  for (const int32_t pid : cfg.target_pid()) {
    filter.pids.insert(pid);
  }
  for (const int32_t pid : cfg.exclude_pid()) {
    filter.exclude_pids.insert(pid);
  }
  filter.additional_cmdline_count = cfg.additional_cmdline_count();
  filter.process_sharding = process_sharding;
  return filter;
}

constexpr bool IsPowerOfTwo(size_t v) {
  return (v != 0 && ((v & (v - 1)) == 0));
}

// returns |std::nullopt| if the input is invalid.
std::optional<uint32_t> ChooseActualRingBufferPages(uint32_t config_value) {
  if (!config_value) {
    static_assert(IsPowerOfTwo(kDefaultDataPagesPerRingBuffer), "");
    return std::make_optional(kDefaultDataPagesPerRingBuffer);
  }

  if (!IsPowerOfTwo(config_value)) {
    PERFETTO_ELOG("kernel buffer size must be a power of two pages");
    return std::nullopt;
  }

  return std::make_optional(config_value);
}

std::optional<PerfCounter> ToPerfCounter(
    std::string name,
    protos::gen::PerfEvents::Counter pb_enum) {
  using protos::gen::PerfEvents;
  switch (static_cast<int>(pb_enum)) {  // cast to pacify -Wswitch-enum
    case PerfEvents::SW_CPU_CLOCK:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_CPU_CLOCK,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_CPU_CLOCK);
    case PerfEvents::SW_PAGE_FAULTS:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_PAGE_FAULTS,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_PAGE_FAULTS);
    case PerfEvents::SW_TASK_CLOCK:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_TASK_CLOCK,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_TASK_CLOCK);
    case PerfEvents::SW_CONTEXT_SWITCHES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_CONTEXT_SWITCHES,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_CONTEXT_SWITCHES);
    case PerfEvents::SW_CPU_MIGRATIONS:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_CPU_MIGRATIONS,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_CPU_MIGRATIONS);
    case PerfEvents::SW_PAGE_FAULTS_MIN:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_PAGE_FAULTS_MIN,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_PAGE_FAULTS_MIN);
    case PerfEvents::SW_PAGE_FAULTS_MAJ:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_PAGE_FAULTS_MAJ,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_PAGE_FAULTS_MAJ);
    case PerfEvents::SW_ALIGNMENT_FAULTS:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_ALIGNMENT_FAULTS,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_ALIGNMENT_FAULTS);
    case PerfEvents::SW_EMULATION_FAULTS:
      return PerfCounter::BuiltinCounter(name, PerfEvents::SW_EMULATION_FAULTS,
                                         PERF_TYPE_SOFTWARE,
                                         PERF_COUNT_SW_EMULATION_FAULTS);
    case PerfEvents::SW_DUMMY:
      return PerfCounter::BuiltinCounter(
          name, PerfEvents::SW_DUMMY, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_DUMMY);

    case PerfEvents::HW_CPU_CYCLES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_CPU_CYCLES,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_CPU_CYCLES);
    case PerfEvents::HW_INSTRUCTIONS:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_INSTRUCTIONS,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_INSTRUCTIONS);
    case PerfEvents::HW_CACHE_REFERENCES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_CACHE_REFERENCES,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_CACHE_REFERENCES);
    case PerfEvents::HW_CACHE_MISSES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_CACHE_MISSES,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_CACHE_MISSES);
    case PerfEvents::HW_BRANCH_INSTRUCTIONS:
      return PerfCounter::BuiltinCounter(
          name, PerfEvents::HW_BRANCH_INSTRUCTIONS, PERF_TYPE_HARDWARE,
          PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
    case PerfEvents::HW_BRANCH_MISSES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_BRANCH_MISSES,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_BRANCH_MISSES);
    case PerfEvents::HW_BUS_CYCLES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_BUS_CYCLES,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_BUS_CYCLES);
    case PerfEvents::HW_STALLED_CYCLES_FRONTEND:
      return PerfCounter::BuiltinCounter(
          name, PerfEvents::HW_STALLED_CYCLES_FRONTEND, PERF_TYPE_HARDWARE,
          PERF_COUNT_HW_STALLED_CYCLES_FRONTEND);
    case PerfEvents::HW_STALLED_CYCLES_BACKEND:
      return PerfCounter::BuiltinCounter(
          name, PerfEvents::HW_STALLED_CYCLES_BACKEND, PERF_TYPE_HARDWARE,
          PERF_COUNT_HW_STALLED_CYCLES_BACKEND);
    case PerfEvents::HW_REF_CPU_CYCLES:
      return PerfCounter::BuiltinCounter(name, PerfEvents::HW_REF_CPU_CYCLES,
                                         PERF_TYPE_HARDWARE,
                                         PERF_COUNT_HW_REF_CPU_CYCLES);

    default:
      PERFETTO_ELOG("Unrecognised PerfEvents::Counter enum value: %zu",
                    static_cast<size_t>(pb_enum));
      return std::nullopt;
  }
}

int32_t ToClockId(protos::gen::PerfEvents::PerfClock pb_enum) {
  using protos::gen::PerfEvents;
  switch (static_cast<int>(pb_enum)) {  // cast to pacify -Wswitch-enum
    case PerfEvents::PERF_CLOCK_REALTIME:
      return CLOCK_REALTIME;
    case PerfEvents::PERF_CLOCK_MONOTONIC:
      return CLOCK_MONOTONIC;
    case PerfEvents::PERF_CLOCK_MONOTONIC_RAW:
      return CLOCK_MONOTONIC_RAW;
    case PerfEvents::PERF_CLOCK_BOOTTIME:
      return CLOCK_BOOTTIME;
    // Default to a monotonic clock since it should be compatible with all types
    // of events. Whereas boottime cannot be used with hardware events due to
    // potential access within non-maskable interrupts.
    default:
      return CLOCK_MONOTONIC_RAW;
  }
}

}  // namespace

// static
PerfCounter PerfCounter::BuiltinCounter(
    std::string name,
    protos::gen::PerfEvents::Counter counter,
    uint32_t type,
    uint64_t config) {
  PerfCounter ret;
  ret.type = PerfCounter::Type::kBuiltinCounter;
  ret.counter = counter;
  ret.name = std::move(name);

  ret.attr_type = type;
  ret.attr_config = config;
  // none of the builtin counters require config1 and config2 at the moment
  return ret;
}

// static
PerfCounter PerfCounter::Tracepoint(std::string name,
                                    std::string tracepoint_name,
                                    std::string tracepoint_filter,
                                    uint64_t id) {
  PerfCounter ret;
  ret.type = PerfCounter::Type::kTracepoint;
  ret.tracepoint_name = std::move(tracepoint_name);
  ret.tracepoint_filter = std::move(tracepoint_filter);
  ret.name = std::move(name);

  ret.attr_type = PERF_TYPE_TRACEPOINT;
  ret.attr_config = id;
  return ret;
}

// static
PerfCounter PerfCounter::RawEvent(std::string name,
                                  uint32_t type,
                                  uint64_t config,
                                  uint64_t config1,
                                  uint64_t config2) {
  PerfCounter ret;
  ret.type = PerfCounter::Type::kRawEvent;
  ret.name = std::move(name);

  ret.attr_type = type;
  ret.attr_config = config;
  ret.attr_config1 = config1;
  ret.attr_config2 = config2;
  return ret;
}

// static
std::optional<EventConfig> EventConfig::Create(
    const protos::gen::PerfEventConfig& pb_config,
    const DataSourceConfig& raw_ds_config,
    std::optional<ProcessSharding> process_sharding,
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
  std::string timebase_name = pb_config.timebase().name();
  if (pb_config.timebase().has_counter()) {
    auto maybe_counter =
        ToPerfCounter(timebase_name, pb_config.timebase().counter());
    if (!maybe_counter)
      return std::nullopt;
    timebase_event = *maybe_counter;

  } else if (pb_config.timebase().has_tracepoint()) {
    const auto& tracepoint_pb = pb_config.timebase().tracepoint();
    std::optional<uint32_t> maybe_id =
        ParseTracepointAndResolveId(tracepoint_pb, tracepoint_id_lookup);
    if (!maybe_id)
      return std::nullopt;
    timebase_event = PerfCounter::Tracepoint(
        timebase_name, tracepoint_pb.name(), tracepoint_pb.filter(), *maybe_id);

  } else if (pb_config.timebase().has_raw_event()) {
    const auto& raw = pb_config.timebase().raw_event();
    timebase_event = PerfCounter::RawEvent(
        timebase_name, raw.type(), raw.config(), raw.config1(), raw.config2());

  } else {
    timebase_event = PerfCounter::BuiltinCounter(
        timebase_name, protos::gen::PerfEvents::PerfEvents::SW_CPU_CLOCK,
        PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  }

  // Callstack sampling.
  bool user_frames = false;
  bool kernel_frames = false;
  TargetFilter target_filter;
  bool legacy_config = pb_config.all_cpus();  // all_cpus was mandatory before
  if (pb_config.has_callstack_sampling() || legacy_config) {
    user_frames = true;

    // Userspace callstacks.
    using protos::gen::PerfEventConfig;
    switch (static_cast<int>(pb_config.callstack_sampling().user_frames())) {
      case PerfEventConfig::UNWIND_UNKNOWN:
        // default to true, both for backwards compatibility and because it's
        // almost always what the user wants.
        user_frames = true;
        break;
      case PerfEventConfig::UNWIND_SKIP:
        user_frames = false;
        break;
      case PerfEventConfig::UNWIND_DWARF:
        user_frames = true;
        break;
      default:
        // enum value from the future that we don't yet know, refuse the config
        // TODO(rsavitski): double-check that both pbzero and ::gen propagate
        // unknown enum values.
        return std::nullopt;
    }

    // Process scoping. Sharding parameter is supplied from outside as it is
    // shared by all data sources within a tracing session.
    target_filter =
        pb_config.callstack_sampling().has_scope()
            ? ParseTargetFilter(pb_config.callstack_sampling().scope(),
                                process_sharding)
            : ParseTargetFilter(pb_config,
                                process_sharding);  // backwards compatibility

    // Kernel callstacks.
    kernel_frames = pb_config.callstack_sampling().kernel_frames() ||
                    pb_config.kernel_frames();
  }

  // Ring buffer options.
  std::optional<uint32_t> ring_buffer_pages =
      ChooseActualRingBufferPages(pb_config.ring_buffer_pages());
  if (!ring_buffer_pages.has_value())
    return std::nullopt;

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
    // Double the limit to account of actual sample rate uncertainties, as
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
    return std::nullopt;

  // Optional footprint controls.
  uint64_t max_enqueued_footprint_bytes =
      pb_config.max_enqueued_footprint_kb() * 1024;

  // Android-specific options.
  uint32_t remote_descriptor_timeout_ms =
      pb_config.remote_descriptor_timeout_ms()
          ? pb_config.remote_descriptor_timeout_ms()
          : kDefaultRemoteDescriptorTimeoutMs;

  // Build the underlying syscall config struct.
  perf_event_attr pe = {};
  pe.size = sizeof(perf_event_attr);
  pe.disabled = 1;  // will be activated via ioctl

  // Sampling timebase.
  pe.type = timebase_event.attr_type;
  pe.config = timebase_event.attr_config;
  pe.config1 = timebase_event.attr_config1;
  pe.config2 = timebase_event.attr_config2;
  if (sampling_frequency) {
    pe.freq = true;
    pe.sample_freq = sampling_frequency;
  } else {
    pe.sample_period = sampling_period;
  }

  // What the samples will contain.
  pe.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_READ;
  // PERF_SAMPLE_TIME:
  pe.clockid = ToClockId(pb_config.timebase().timestamp_clock());
  pe.use_clockid = true;

  if (user_frames) {
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
  }
  if (kernel_frames) {
    pe.sample_type |= PERF_SAMPLE_CALLCHAIN;
    pe.exclude_callchain_user = true;
  }

  return EventConfig(
      raw_ds_config, pe, timebase_event, user_frames, kernel_frames,
      std::move(target_filter), ring_buffer_pages.value(), read_tick_period_ms,
      samples_per_tick_limit, remote_descriptor_timeout_ms,
      pb_config.unwind_state_clear_period_ms(), max_enqueued_footprint_bytes,
      pb_config.target_installed_by());
}

EventConfig::EventConfig(const DataSourceConfig& raw_ds_config,
                         const perf_event_attr& pe,
                         const PerfCounter& timebase_event,
                         bool user_frames,
                         bool kernel_frames,
                         TargetFilter target_filter,
                         uint32_t ring_buffer_pages,
                         uint32_t read_tick_period_ms,
                         uint64_t samples_per_tick_limit,
                         uint32_t remote_descriptor_timeout_ms,
                         uint32_t unwind_state_clear_period_ms,
                         uint64_t max_enqueued_footprint_bytes,
                         std::vector<std::string> target_installed_by)
    : perf_event_attr_(pe),
      timebase_event_(timebase_event),
      user_frames_(user_frames),
      kernel_frames_(kernel_frames),
      target_filter_(std::move(target_filter)),
      ring_buffer_pages_(ring_buffer_pages),
      read_tick_period_ms_(read_tick_period_ms),
      samples_per_tick_limit_(samples_per_tick_limit),
      remote_descriptor_timeout_ms_(remote_descriptor_timeout_ms),
      unwind_state_clear_period_ms_(unwind_state_clear_period_ms),
      max_enqueued_footprint_bytes_(max_enqueued_footprint_bytes),
      target_installed_by_(std::move(target_installed_by)),
      raw_ds_config_(raw_ds_config) /* full copy */ {}

}  // namespace profiling
}  // namespace perfetto
