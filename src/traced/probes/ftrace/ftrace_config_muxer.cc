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

#include "src/traced/probes/ftrace/ftrace_config_muxer.h"

#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdint>

#include <algorithm>
#include <iterator>
#include <limits>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "src/traced/probes/ftrace/atrace_wrapper.h"
#include "src/traced/probes/ftrace/compact_sched.h"
#include "src/traced/probes/ftrace/ftrace_config_utils.h"
#include "src/traced/probes/ftrace/ftrace_stats.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"

namespace perfetto {
namespace {

using protos::pbzero::KprobeEvent;

constexpr uint64_t kDefaultLowRamPerCpuBufferSizeKb = 2 * (1ULL << 10);   // 2mb
constexpr uint64_t kDefaultHighRamPerCpuBufferSizeKb = 8 * (1ULL << 10);  // 8mb

// Threshold for physical ram size used when deciding on default kernel buffer
// sizes. We want to detect 8 GB, but the size reported through sysconf is
// usually lower.
constexpr uint64_t kHighMemBytes = 7 * (1ULL << 30);  // 7gb

// A fake "syscall id" that indicates all syscalls should be recorded. This
// allows us to distinguish between the case where `syscall_events` is empty
// because raw_syscalls aren't enabled, or the case where it is and we want to
// record all events.
constexpr size_t kAllSyscallsId = kMaxSyscalls + 1;

// trace_clocks in preference order.
// If this list is changed, the FtraceClocks enum in ftrace_event_bundle.proto
// and FtraceConfigMuxer::SetupClock() should be also changed accordingly.
constexpr const char* kClocks[] = {"boot", "global", "local"};

// optional monotonic raw clock.
// Enabled by the "use_monotonic_raw_clock" option in the ftrace config.
constexpr const char* kClockMonoRaw = "mono_raw";

void AddEventGroup(const ProtoTranslationTable* table,
                   const std::string& group,
                   std::set<GroupAndName>* to) {
  const std::vector<const Event*>* events = table->GetEventsByGroup(group);
  if (!events)
    return;
  for (const Event* event : *events)
    to->insert(GroupAndName(group, event->name));
}

std::set<GroupAndName> ReadEventsInGroupFromFs(
    const FtraceProcfs& ftrace_procfs,
    const std::string& group) {
  std::set<std::string> names =
      ftrace_procfs.GetEventNamesForGroup("events/" + group);
  std::set<GroupAndName> events;
  for (const auto& name : names)
    events.insert(GroupAndName(group, name));
  return events;
}

std::pair<std::string, std::string> EventToStringGroupAndName(
    const std::string& event) {
  auto slash_pos = event.find('/');
  if (slash_pos == std::string::npos)
    return std::make_pair("", event);
  return std::make_pair(event.substr(0, slash_pos),
                        event.substr(slash_pos + 1));
}

void UnionInPlace(const std::vector<std::string>& unsorted_a,
                  std::vector<std::string>* out) {
  std::vector<std::string> a = unsorted_a;
  std::sort(a.begin(), a.end());
  std::sort(out->begin(), out->end());
  std::vector<std::string> v;
  std::set_union(a.begin(), a.end(), out->begin(), out->end(),
                 std::back_inserter(v));
  *out = std::move(v);
}

void IntersectInPlace(const std::vector<std::string>& unsorted_a,
                      std::vector<std::string>* out) {
  std::vector<std::string> a = unsorted_a;
  std::sort(a.begin(), a.end());
  std::sort(out->begin(), out->end());
  std::vector<std::string> v;
  std::set_intersection(a.begin(), a.end(), out->begin(), out->end(),
                        std::back_inserter(v));
  *out = std::move(v);
}

std::vector<std::string> Subtract(const std::vector<std::string>& unsorted_a,
                                  const std::vector<std::string>& unsorted_b) {
  std::vector<std::string> a = unsorted_a;
  std::sort(a.begin(), a.end());
  std::vector<std::string> b = unsorted_b;
  std::sort(b.begin(), b.end());
  std::vector<std::string> v;
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                      std::back_inserter(v));
  return v;
}

// This is just to reduce binary size and stack frame size of the insertions.
// It effectively undoes STL's set::insert inlining.
void PERFETTO_NO_INLINE InsertEvent(const char* group,
                                    const char* name,
                                    std::set<GroupAndName>* dst) {
  dst->insert(GroupAndName(group, name));
}

std::map<GroupAndName, KprobeEvent::KprobeType> GetFtraceKprobeEvents(
    const FtraceConfig& request) {
  std::map<GroupAndName, KprobeEvent::KprobeType> events;
  for (const auto& config_value : request.kprobe_events()) {
    switch (config_value.type()) {
      case protos::gen::FtraceConfig::KprobeEvent::KPROBE_TYPE_KPROBE:
        events[GroupAndName(kKprobeGroup, config_value.probe().c_str())] =
            KprobeEvent::KprobeType::KPROBE_TYPE_INSTANT;
        break;
      case protos::gen::FtraceConfig::KprobeEvent::KPROBE_TYPE_KRETPROBE:
        events[GroupAndName(kKretprobeGroup, config_value.probe().c_str())] =
            KprobeEvent::KprobeType::KPROBE_TYPE_INSTANT;
        break;
      case protos::gen::FtraceConfig::KprobeEvent::KPROBE_TYPE_BOTH:
        events[GroupAndName(kKprobeGroup, config_value.probe().c_str())] =
            KprobeEvent::KprobeType::KPROBE_TYPE_BEGIN;
        events[GroupAndName(kKretprobeGroup, config_value.probe().c_str())] =
            KprobeEvent::KprobeType::KPROBE_TYPE_END;
        break;
      case protos::gen::FtraceConfig::KprobeEvent::KPROBE_TYPE_UNKNOWN:
        PERFETTO_DLOG("Unknown kprobe event");
        break;
    }
    PERFETTO_DLOG("Added kprobe event: %s", config_value.probe().c_str());
  }
  return events;
}

bool ValidateKprobeName(const std::string& name) {
  for (const char& c : name) {
    if (!std::isalnum(c) && c != '_') {
      return false;
    }
  }
  return true;
}

}  // namespace

std::set<GroupAndName> FtraceConfigMuxer::GetFtraceEvents(
    const FtraceConfig& request,
    const ProtoTranslationTable* table) {
  std::set<GroupAndName> events;
  for (const auto& config_value : request.ftrace_events()) {
    std::string group;
    std::string name;
    std::tie(group, name) = EventToStringGroupAndName(config_value);
    if (name == "*") {
      for (const auto& event : ReadEventsInGroupFromFs(*ftrace_, group))
        events.insert(event);
    } else if (group.empty()) {
      // If there is no group specified, find an event with that name and
      // use it's group.
      const Event* e = table->GetEventByName(name);
      if (!e) {
        PERFETTO_DLOG(
            "Event doesn't exist: %s. Include the group in the config to allow "
            "the event to be output as a generic event.",
            name.c_str());
        continue;
      }
      events.insert(GroupAndName(e->group, e->name));
    } else {
      events.insert(GroupAndName(group, name));
    }
  }
  if (RequiresAtrace(request)) {
    InsertEvent("ftrace", "print", &events);

    // Ideally we should keep this code in sync with:
    // platform/frameworks/native/cmds/atrace/atrace.cpp
    // It's not a disaster if they go out of sync, we can always add the ftrace
    // categories manually server side but this is user friendly and reduces the
    // size of the configs.
    for (const std::string& category : request.atrace_categories()) {
      if (category == "gfx") {
        AddEventGroup(table, "mdss", &events);
        InsertEvent("mdss", "rotator_bw_ao_as_context", &events);
        InsertEvent("mdss", "mdp_trace_counter", &events);
        InsertEvent("mdss", "tracing_mark_write", &events);
        InsertEvent("mdss", "mdp_cmd_wait_pingpong", &events);
        InsertEvent("mdss", "mdp_cmd_kickoff", &events);
        InsertEvent("mdss", "mdp_cmd_release_bw", &events);
        InsertEvent("mdss", "mdp_cmd_readptr_done", &events);
        InsertEvent("mdss", "mdp_cmd_pingpong_done", &events);
        InsertEvent("mdss", "mdp_misr_crc", &events);
        InsertEvent("mdss", "mdp_compare_bw", &events);
        InsertEvent("mdss", "mdp_perf_update_bus", &events);
        InsertEvent("mdss", "mdp_video_underrun_done", &events);
        InsertEvent("mdss", "mdp_commit", &events);
        InsertEvent("mdss", "mdp_mixer_update", &events);
        InsertEvent("mdss", "mdp_perf_prefill_calc", &events);
        InsertEvent("mdss", "mdp_perf_set_ot", &events);
        InsertEvent("mdss", "mdp_perf_set_wm_levels", &events);
        InsertEvent("mdss", "mdp_perf_set_panic_luts", &events);
        InsertEvent("mdss", "mdp_perf_set_qos_luts", &events);
        InsertEvent("mdss", "mdp_sspp_change", &events);
        InsertEvent("mdss", "mdp_sspp_set", &events);
        AddEventGroup(table, "mali", &events);
        InsertEvent("mali", "tracing_mark_write", &events);

        AddEventGroup(table, "sde", &events);
        InsertEvent("sde", "tracing_mark_write", &events);
        InsertEvent("sde", "sde_perf_update_bus", &events);
        InsertEvent("sde", "sde_perf_set_qos_luts", &events);
        InsertEvent("sde", "sde_perf_set_ot", &events);
        InsertEvent("sde", "sde_perf_set_danger_luts", &events);
        InsertEvent("sde", "sde_perf_crtc_update", &events);
        InsertEvent("sde", "sde_perf_calc_crtc", &events);
        InsertEvent("sde", "sde_evtlog", &events);
        InsertEvent("sde", "sde_encoder_underrun", &events);
        InsertEvent("sde", "sde_cmd_release_bw", &events);

        AddEventGroup(table, "dpu", &events);
        InsertEvent("dpu", "tracing_mark_write", &events);

        AddEventGroup(table, "g2d", &events);
        InsertEvent("g2d", "tracing_mark_write", &events);
        InsertEvent("g2d", "g2d_perf_update_qos", &events);

        AddEventGroup(table, "panel", &events);
        InsertEvent("panel", "panel_write_generic", &events);
        continue;
      }

      if (category == "ion") {
        InsertEvent("kmem", "ion_alloc_buffer_start", &events);
        continue;
      }

      // Note: sched_wakeup intentionally removed (diverging from atrace), as it
      // is high-volume, but mostly redundant when sched_waking is also enabled.
      // The event can still be enabled explicitly when necessary.
      if (category == "sched") {
        InsertEvent("sched", "sched_switch", &events);
        InsertEvent("sched", "sched_waking", &events);
        InsertEvent("sched", "sched_blocked_reason", &events);
        InsertEvent("sched", "sched_cpu_hotplug", &events);
        InsertEvent("sched", "sched_pi_setprio", &events);
        InsertEvent("sched", "sched_process_exit", &events);
        AddEventGroup(table, "cgroup", &events);
        InsertEvent("cgroup", "cgroup_transfer_tasks", &events);
        InsertEvent("cgroup", "cgroup_setup_root", &events);
        InsertEvent("cgroup", "cgroup_rmdir", &events);
        InsertEvent("cgroup", "cgroup_rename", &events);
        InsertEvent("cgroup", "cgroup_remount", &events);
        InsertEvent("cgroup", "cgroup_release", &events);
        InsertEvent("cgroup", "cgroup_mkdir", &events);
        InsertEvent("cgroup", "cgroup_destroy_root", &events);
        InsertEvent("cgroup", "cgroup_attach_task", &events);
        InsertEvent("oom", "oom_score_adj_update", &events);
        InsertEvent("task", "task_rename", &events);
        InsertEvent("task", "task_newtask", &events);

        AddEventGroup(table, "systrace", &events);
        InsertEvent("systrace", "0", &events);

        AddEventGroup(table, "scm", &events);
        InsertEvent("scm", "scm_call_start", &events);
        InsertEvent("scm", "scm_call_end", &events);
        continue;
      }

      if (category == "irq") {
        AddEventGroup(table, "irq", &events);
        InsertEvent("irq", "tasklet_hi_exit", &events);
        InsertEvent("irq", "tasklet_hi_entry", &events);
        InsertEvent("irq", "tasklet_exit", &events);
        InsertEvent("irq", "tasklet_entry", &events);
        InsertEvent("irq", "softirq_raise", &events);
        InsertEvent("irq", "softirq_exit", &events);
        InsertEvent("irq", "softirq_entry", &events);
        InsertEvent("irq", "irq_handler_exit", &events);
        InsertEvent("irq", "irq_handler_entry", &events);
        AddEventGroup(table, "ipi", &events);
        InsertEvent("ipi", "ipi_raise", &events);
        InsertEvent("ipi", "ipi_exit", &events);
        InsertEvent("ipi", "ipi_entry", &events);
        continue;
      }

      if (category == "irqoff") {
        InsertEvent("preemptirq", "irq_enable", &events);
        InsertEvent("preemptirq", "irq_disable", &events);
        continue;
      }

      if (category == "preemptoff") {
        InsertEvent("preemptirq", "preempt_enable", &events);
        InsertEvent("preemptirq", "preempt_disable", &events);
        continue;
      }

      if (category == "i2c") {
        AddEventGroup(table, "i2c", &events);
        InsertEvent("i2c", "i2c_read", &events);
        InsertEvent("i2c", "i2c_write", &events);
        InsertEvent("i2c", "i2c_result", &events);
        InsertEvent("i2c", "i2c_reply", &events);
        InsertEvent("i2c", "smbus_read", &events);
        InsertEvent("i2c", "smbus_write", &events);
        InsertEvent("i2c", "smbus_result", &events);
        InsertEvent("i2c", "smbus_reply", &events);
        continue;
      }

      if (category == "freq") {
        InsertEvent("power", "cpu_frequency", &events);
        InsertEvent("power", "gpu_frequency", &events);
        InsertEvent("power", "clock_set_rate", &events);
        InsertEvent("power", "clock_disable", &events);
        InsertEvent("power", "clock_enable", &events);
        InsertEvent("clk", "clk_set_rate", &events);
        InsertEvent("clk", "clk_disable", &events);
        InsertEvent("clk", "clk_enable", &events);
        InsertEvent("power", "cpu_frequency_limits", &events);
        InsertEvent("power", "suspend_resume", &events);
        InsertEvent("cpuhp", "cpuhp_enter", &events);
        InsertEvent("cpuhp", "cpuhp_exit", &events);
        InsertEvent("cpuhp", "cpuhp_pause", &events);
        AddEventGroup(table, "msm_bus", &events);
        InsertEvent("msm_bus", "bus_update_request_end", &events);
        InsertEvent("msm_bus", "bus_update_request", &events);
        InsertEvent("msm_bus", "bus_rules_matches", &events);
        InsertEvent("msm_bus", "bus_max_votes", &events);
        InsertEvent("msm_bus", "bus_client_status", &events);
        InsertEvent("msm_bus", "bus_bke_params", &events);
        InsertEvent("msm_bus", "bus_bimc_config_limiter", &events);
        InsertEvent("msm_bus", "bus_avail_bw", &events);
        InsertEvent("msm_bus", "bus_agg_bw", &events);
        continue;
      }

      if (category == "membus") {
        AddEventGroup(table, "memory_bus", &events);
        continue;
      }

      if (category == "idle") {
        InsertEvent("power", "cpu_idle", &events);
        continue;
      }

      if (category == "disk") {
        InsertEvent("f2fs", "f2fs_sync_file_enter", &events);
        InsertEvent("f2fs", "f2fs_sync_file_exit", &events);
        InsertEvent("f2fs", "f2fs_write_begin", &events);
        InsertEvent("f2fs", "f2fs_write_end", &events);
        InsertEvent("f2fs", "f2fs_iostat", &events);
        InsertEvent("f2fs", "f2fs_iostat_latency", &events);
        InsertEvent("ext4", "ext4_da_write_begin", &events);
        InsertEvent("ext4", "ext4_da_write_end", &events);
        InsertEvent("ext4", "ext4_sync_file_enter", &events);
        InsertEvent("ext4", "ext4_sync_file_exit", &events);
        InsertEvent("block", "block_bio_queue", &events);
        InsertEvent("block", "block_bio_complete", &events);
        InsertEvent("ufs", "ufshcd_command", &events);
        continue;
      }

      if (category == "mmc") {
        AddEventGroup(table, "mmc", &events);
        continue;
      }

      if (category == "load") {
        AddEventGroup(table, "cpufreq_interactive", &events);
        continue;
      }

      if (category == "sync") {
        // linux kernel < 4.9
        AddEventGroup(table, "sync", &events);
        InsertEvent("sync", "sync_pt", &events);
        InsertEvent("sync", "sync_timeline", &events);
        InsertEvent("sync", "sync_wait", &events);
        // linux kernel == 4.9.x
        AddEventGroup(table, "fence", &events);
        InsertEvent("fence", "fence_annotate_wait_on", &events);
        InsertEvent("fence", "fence_destroy", &events);
        InsertEvent("fence", "fence_emit", &events);
        InsertEvent("fence", "fence_enable_signal", &events);
        InsertEvent("fence", "fence_init", &events);
        InsertEvent("fence", "fence_signaled", &events);
        InsertEvent("fence", "fence_wait_end", &events);
        InsertEvent("fence", "fence_wait_start", &events);
        // linux kernel > 4.9
        AddEventGroup(table, "dma_fence", &events);
        continue;
      }

      if (category == "workq") {
        AddEventGroup(table, "workqueue", &events);
        InsertEvent("workqueue", "workqueue_queue_work", &events);
        InsertEvent("workqueue", "workqueue_execute_start", &events);
        InsertEvent("workqueue", "workqueue_execute_end", &events);
        InsertEvent("workqueue", "workqueue_activate_work", &events);
        continue;
      }

      if (category == "memreclaim") {
        InsertEvent("vmscan", "mm_vmscan_direct_reclaim_begin", &events);
        InsertEvent("vmscan", "mm_vmscan_direct_reclaim_end", &events);
        InsertEvent("vmscan", "mm_vmscan_kswapd_wake", &events);
        InsertEvent("vmscan", "mm_vmscan_kswapd_sleep", &events);
        AddEventGroup(table, "lowmemorykiller", &events);
        InsertEvent("lowmemorykiller", "lowmemory_kill", &events);
        continue;
      }

      if (category == "regulators") {
        AddEventGroup(table, "regulator", &events);
        events.insert(
            GroupAndName("regulator", "regulator_set_voltage_complete"));
        InsertEvent("regulator", "regulator_set_voltage", &events);
        InsertEvent("regulator", "regulator_enable_delay", &events);
        InsertEvent("regulator", "regulator_enable_complete", &events);
        InsertEvent("regulator", "regulator_enable", &events);
        InsertEvent("regulator", "regulator_disable_complete", &events);
        InsertEvent("regulator", "regulator_disable", &events);
        continue;
      }

      if (category == "binder_driver") {
        InsertEvent("binder", "binder_transaction", &events);
        InsertEvent("binder", "binder_transaction_received", &events);
        InsertEvent("binder", "binder_transaction_alloc_buf", &events);
        InsertEvent("binder", "binder_set_priority", &events);
        continue;
      }

      if (category == "binder_lock") {
        InsertEvent("binder", "binder_lock", &events);
        InsertEvent("binder", "binder_locked", &events);
        InsertEvent("binder", "binder_unlock", &events);
        continue;
      }

      if (category == "pagecache") {
        AddEventGroup(table, "filemap", &events);
        events.insert(
            GroupAndName("filemap", "mm_filemap_delete_from_page_cache"));
        InsertEvent("filemap", "mm_filemap_add_to_page_cache", &events);
        InsertEvent("filemap", "filemap_set_wb_err", &events);
        InsertEvent("filemap", "file_check_and_advance_wb_err", &events);
        continue;
      }

      if (category == "memory") {
        // Use rss_stat_throttled if supported
        if (ftrace_->SupportsRssStatThrottled()) {
          InsertEvent("synthetic", "rss_stat_throttled", &events);
        } else {
          InsertEvent("kmem", "rss_stat", &events);
        }
        InsertEvent("kmem", "ion_heap_grow", &events);
        InsertEvent("kmem", "ion_heap_shrink", &events);
        // ion_stat supersedes ion_heap_grow / shrink for kernel 4.19+
        InsertEvent("ion", "ion_stat", &events);
        InsertEvent("mm_event", "mm_event_record", &events);
        InsertEvent("dmabuf_heap", "dma_heap_stat", &events);
        InsertEvent("gpu_mem", "gpu_mem_total", &events);
        continue;
      }

      if (category == "thermal") {
        InsertEvent("thermal", "thermal_temperature", &events);
        InsertEvent("thermal", "cdev_update", &events);
        continue;
      }

      if (category == "camera") {
        AddEventGroup(table, "lwis", &events);
        InsertEvent("lwis", "tracing_mark_write", &events);
        continue;
      }
    }
  }

  // recording a subset of syscalls -> enable the backing events
  if (request.syscall_events_size() > 0) {
    InsertEvent("raw_syscalls", "sys_enter", &events);
    InsertEvent("raw_syscalls", "sys_exit", &events);
  }

  // function_graph tracer emits two builtin ftrace events
  if (request.enable_function_graph()) {
    InsertEvent("ftrace", "funcgraph_entry", &events);
    InsertEvent("ftrace", "funcgraph_exit", &events);
  }

  // If throttle_rss_stat: true, use the rss_stat_throttled event if supported
  if (request.throttle_rss_stat() && ftrace_->SupportsRssStatThrottled()) {
    auto it = std::find_if(
        events.begin(), events.end(), [](const GroupAndName& event) {
          return event.group() == "kmem" && event.name() == "rss_stat";
        });

    if (it != events.end()) {
      events.erase(it);
      InsertEvent("synthetic", "rss_stat_throttled", &events);
    }
  }

  return events;
}

base::FlatSet<int64_t> FtraceConfigMuxer::GetSyscallsReturningFds(
    const SyscallTable& syscalls) {
  auto insertSyscallId = [&syscalls](base::FlatSet<int64_t>& set,
                                     const char* syscall) {
    auto syscall_id = syscalls.GetByName(syscall);
    if (syscall_id)
      set.insert(static_cast<int64_t>(*syscall_id));
  };

  base::FlatSet<int64_t> call_ids;
  insertSyscallId(call_ids, "sys_open");
  insertSyscallId(call_ids, "sys_openat");
  insertSyscallId(call_ids, "sys_socket");
  insertSyscallId(call_ids, "sys_dup");
  insertSyscallId(call_ids, "sys_dup2");
  insertSyscallId(call_ids, "sys_dup3");
  return call_ids;
}

bool FtraceConfigMuxer::FilterHasGroup(const EventFilter& filter,
                                       const std::string& group) {
  const std::vector<const Event*>* events = table_->GetEventsByGroup(group);
  if (!events) {
    return false;
  }

  for (const Event* event : *events) {
    if (filter.IsEventEnabled(event->ftrace_event_id)) {
      return true;
    }
  }
  return false;
}

EventFilter FtraceConfigMuxer::BuildSyscallFilter(
    const EventFilter& ftrace_filter,
    const FtraceConfig& request) {
  EventFilter output;

  if (!FilterHasGroup(ftrace_filter, "raw_syscalls")) {
    return output;
  }

  if (request.syscall_events().empty()) {
    output.AddEnabledEvent(kAllSyscallsId);
    return output;
  }

  for (const std::string& syscall : request.syscall_events()) {
    std::optional<size_t> id = syscalls_.GetByName(syscall);
    if (!id.has_value()) {
      PERFETTO_ELOG("Can't enable %s, syscall not known", syscall.c_str());
      continue;
    }
    output.AddEnabledEvent(*id);
  }

  return output;
}

bool FtraceConfigMuxer::SetSyscallEventFilter(
    const EventFilter& extra_syscalls) {
  EventFilter syscall_filter;

  syscall_filter.EnableEventsFrom(extra_syscalls);
  for (const auto& id_config : ds_configs_) {
    const perfetto::FtraceDataSourceConfig& config = id_config.second;
    syscall_filter.EnableEventsFrom(config.syscall_filter);
  }

  std::set<size_t> filter_set = syscall_filter.GetEnabledEvents();
  if (syscall_filter.IsEventEnabled(kAllSyscallsId)) {
    filter_set.clear();
  }

  if (current_state_.syscall_filter != filter_set) {
    if (!ftrace_->SetSyscallFilter(filter_set)) {
      return false;
    }

    current_state_.syscall_filter = filter_set;
  }

  return true;
}

void FtraceConfigMuxer::EnableFtraceEvent(const Event* event,
                                          const GroupAndName& group_and_name,
                                          EventFilter* filter,
                                          FtraceSetupErrors* errors) {
  // Note: ftrace events are always implicitly enabled (and don't have an
  // "enable" file). So they aren't tracked by the central event filter (but
  // still need to be added to the per data source event filter to retain
  // the events during parsing).
  if (current_state_.ftrace_events.IsEventEnabled(event->ftrace_event_id) ||
      std::string("ftrace") == event->group) {
    filter->AddEnabledEvent(event->ftrace_event_id);
    return;
  }
  if (ftrace_->EnableEvent(event->group, event->name)) {
    current_state_.ftrace_events.AddEnabledEvent(event->ftrace_event_id);
    filter->AddEnabledEvent(event->ftrace_event_id);
  } else {
    PERFETTO_DPLOG("Failed to enable %s.", group_and_name.ToString().c_str());
    if (errors)
      errors->failed_ftrace_events.push_back(group_and_name.ToString());
  }
}

FtraceConfigMuxer::FtraceConfigMuxer(
    FtraceProcfs* ftrace,
    AtraceWrapper* atrace_wrapper,
    ProtoTranslationTable* table,
    SyscallTable syscalls,
    std::map<std::string, std::vector<GroupAndName>> vendor_events,
    bool secondary_instance)
    : ftrace_(ftrace),
      atrace_wrapper_(atrace_wrapper),
      table_(table),
      syscalls_(syscalls),
      current_state_(),
      vendor_events_(std::move(vendor_events)),
      secondary_instance_(secondary_instance) {}
FtraceConfigMuxer::~FtraceConfigMuxer() = default;

bool FtraceConfigMuxer::SetupConfig(FtraceConfigId id,
                                    const FtraceConfig& request,
                                    FtraceSetupErrors* errors) {
  EventFilter filter;
  if (ds_configs_.empty()) {
    PERFETTO_DCHECK(active_configs_.empty());

    // If someone outside of perfetto is using a non-nop tracer, yield. We can't
    // realistically figure out all notions of "in use" even if we look at
    // set_event or events/enable, so this is all we check for.
    if (!request.preserve_ftrace_buffer() && !ftrace_->IsTracingAvailable()) {
      PERFETTO_ELOG(
          "ftrace in use by non-Perfetto. Check that %s current_tracer is nop.",
          ftrace_->GetRootPath().c_str());
      return false;
    }

    // Clear tracefs state, remembering which value of "tracing_on" to restore
    // to after we're done, though we won't restore the rest of the tracefs
    // state.
    current_state_.saved_tracing_on = ftrace_->GetTracingOn();
    if (!request.preserve_ftrace_buffer()) {
      ftrace_->SetTracingOn(false);
      // This will fail on release ("user") builds due to ACLs, but that's
      // acceptable since the per-event enabling/disabling should still be
      // balanced.
      ftrace_->DisableAllEvents();
      ftrace_->ClearTrace();
    }

    // Set up the rest of the tracefs state, without starting it.
    // Notes:
    // * resizing buffers can be quite slow (up to hundreds of ms).
    // * resizing buffers may truncate existing contents if the new size is
    // smaller, which matters to the preserve_ftrace_buffer option.
    if (!request.preserve_ftrace_buffer()) {
      SetupClock(request);
      SetupBufferSize(request);
    }
  }

  std::set<GroupAndName> events = GetFtraceEvents(request, table_);
  std::map<GroupAndName, KprobeEvent::KprobeType> events_kprobes =
      GetFtraceKprobeEvents(request);

  // Vendors can provide a set of extra ftrace categories to be enabled when a
  // specific atrace category is used (e.g. "gfx" -> ["my_hw/my_custom_event",
  // "my_hw/my_special_gpu"]). Merge them with the hard coded events for each
  // categories.
  for (const std::string& category : request.atrace_categories()) {
    if (vendor_events_.count(category)) {
      for (const GroupAndName& event : vendor_events_[category]) {
        events.insert(event);
      }
    }
  }

  if (RequiresAtrace(request)) {
    if (secondary_instance_) {
      PERFETTO_ELOG(
          "Secondary ftrace instances do not support atrace_categories and "
          "atrace_apps options as they affect global state");
      return false;
    }
    if (!atrace_wrapper_->SupportsUserspaceOnly() && !ds_configs_.empty()) {
      PERFETTO_ELOG(
          "Concurrent atrace sessions are not supported before Android P, "
          "bailing out.");
      return false;
    }
    UpdateAtrace(request, errors ? &errors->atrace_errors : nullptr);
  }

  base::FlatHashMap<uint32_t, KprobeEvent::KprobeType> kprobes;
  for (const auto& [group_and_name, type] : events_kprobes) {
    if (!ValidateKprobeName(group_and_name.name())) {
      PERFETTO_ELOG("Invalid kprobes event %s", group_and_name.name().c_str());
      if (errors)
        errors->failed_ftrace_events.push_back(group_and_name.ToString());
      continue;
    }
    // Kprobes events are created after their definition is written in the
    // kprobe_events file
    if (!ftrace_->CreateKprobeEvent(
            group_and_name.group(), group_and_name.name(),
            group_and_name.group() == kKretprobeGroup)) {
      PERFETTO_ELOG("Failed creation of kprobes event %s",
                    group_and_name.name().c_str());
      if (errors)
        errors->failed_ftrace_events.push_back(group_and_name.ToString());
      continue;
    }

    const Event* event = table_->GetOrCreateKprobeEvent(group_and_name);
    if (!event) {
      PERFETTO_ELOG("Can't enable kprobe %s",
                    group_and_name.ToString().c_str());
      if (errors)
        errors->unknown_ftrace_events.push_back(group_and_name.ToString());
      continue;
    }
    EnableFtraceEvent(event, group_and_name, &filter, errors);
    kprobes[event->ftrace_event_id] = type;
  }

  for (const auto& group_and_name : events) {
    if (group_and_name.group() == kKprobeGroup ||
        group_and_name.group() == kKretprobeGroup) {
      PERFETTO_DLOG("Can't enable %s, group reserved for kprobes",
                    group_and_name.ToString().c_str());
      if (errors)
        errors->failed_ftrace_events.push_back(group_and_name.ToString());
      continue;
    }
    const Event* event = table_->GetOrCreateEvent(group_and_name);
    if (!event) {
      PERFETTO_DLOG("Can't enable %s, event not known",
                    group_and_name.ToString().c_str());
      if (errors)
        errors->unknown_ftrace_events.push_back(group_and_name.ToString());
      continue;
    }

    // Niche option to skip events that are in the config, but don't have a
    // dedicated proto for the event in perfetto. Otherwise such events will be
    // encoded as GenericFtraceEvent.
    if (request.disable_generic_events() &&
        event->proto_field_id ==
            protos::pbzero::FtraceEvent::kGenericFieldNumber) {
      if (errors)
        errors->failed_ftrace_events.push_back(group_and_name.ToString());
      continue;
    }

    EnableFtraceEvent(event, group_and_name, &filter, errors);
  }

  EventFilter syscall_filter = BuildSyscallFilter(filter, request);
  if (!SetSyscallEventFilter(syscall_filter)) {
    PERFETTO_ELOG("Failed to set raw_syscall ftrace filter in SetupConfig");
    return false;
  }

  // Kernel function tracing (function_graph).
  // Note 1: there is no cleanup in |RemoveConfig| because tracers cannot be
  // changed while tracing pipes are opened. So we'll keep the current_tracer
  // until all data sources are gone, at which point ftrace_controller will
  // make an explicit call to |ResetCurrentTracer|.
  // Note 2: we don't track the set of filters ourselves and instead let the
  // kernel statefully collate them, hence the use of |AppendFunctionFilters|.
  // This is because each concurrent data source that wants funcgraph will get
  // all of the enabled functions (we don't go as far as doing per-DS event
  // steering in the parser), and we don't want to remove functions midway
  // through a trace (but some might get added).
  if (request.enable_function_graph()) {
    if (!current_state_.funcgraph_on && !ftrace_->ClearFunctionFilters())
      return false;
    if (!current_state_.funcgraph_on && !ftrace_->ClearFunctionGraphFilters())
      return false;
    if (!ftrace_->AppendFunctionFilters(request.function_filters()))
      return false;
    if (!ftrace_->AppendFunctionGraphFilters(request.function_graph_roots()))
      return false;
    if (!current_state_.funcgraph_on &&
        !ftrace_->SetCurrentTracer("function_graph")) {
      PERFETTO_LOG(
          "Unable to enable function_graph tracing since a concurrent ftrace "
          "data source is using a different tracer");
      return false;
    }
    current_state_.funcgraph_on = true;
  }
  const auto& compact_format = table_->compact_sched_format();
  auto compact_sched = CreateCompactSchedConfig(
      request, filter.IsEventEnabled(compact_format.sched_switch.event_id),
      compact_format);
  if (errors && !compact_format.format_valid) {
    errors->failed_ftrace_events.emplace_back(
        "perfetto/compact_sched (unexpected sched event format)");
  }

  std::optional<FtracePrintFilterConfig> ftrace_print_filter;
  if (request.has_print_filter()) {
    ftrace_print_filter =
        FtracePrintFilterConfig::Create(request.print_filter(), table_);
    if (!ftrace_print_filter.has_value()) {
      if (errors) {
        errors->failed_ftrace_events.emplace_back(
            "ftrace/print (unexpected format for filtering)");
      }
    }
  }

  std::vector<std::string> apps(request.atrace_apps());
  std::vector<std::string> categories(request.atrace_categories());
  std::vector<std::string> categories_sdk_optout = Subtract(
      request.atrace_categories(), request.atrace_categories_prefer_sdk());
  auto [it, inserted] = ds_configs_.emplace(
      std::piecewise_construct, std::forward_as_tuple(id),
      std::forward_as_tuple(
          std::move(filter), std::move(syscall_filter), compact_sched,
          std::move(ftrace_print_filter), std::move(apps),
          std::move(categories), std::move(categories_sdk_optout),
          request.symbolize_ksyms(), request.drain_buffer_percent(),
          GetSyscallsReturningFds(syscalls_)));
  if (inserted) {
    it->second.kprobes = std::move(kprobes);
  }
  return true;
}

bool FtraceConfigMuxer::ActivateConfig(FtraceConfigId id) {
  if (!id || ds_configs_.count(id) == 0) {
    PERFETTO_DFATAL("Config not found");
    return false;
  }

  bool first_config = active_configs_.empty();
  active_configs_.insert(id);

  // Pick the lowest buffer_percent across the new set of active configs.
  if (!UpdateBufferPercent()) {
    PERFETTO_ELOG(
        "Invalid FtraceConfig.drain_buffer_percent or "
        "/sys/kernel/tracing/buffer_percent file permissions.");
    // carry on, non-critical error
  }

  // Enable kernel event writer.
  if (first_config) {
    if (!ftrace_->SetTracingOn(true)) {
      PERFETTO_ELOG("Failed to enable ftrace.");
      active_configs_.erase(id);
      return false;
    }
  }
  return true;
}

bool FtraceConfigMuxer::RemoveConfig(FtraceConfigId config_id) {
  if (!config_id || !ds_configs_.erase(config_id))
    return false;
  EventFilter expected_ftrace_events;
  std::vector<std::string> expected_apps;
  std::vector<std::string> expected_categories;
  std::vector<std::string> expected_categories_sdk_optout;
  for (const auto& id_config : ds_configs_) {
    const perfetto::FtraceDataSourceConfig& config = id_config.second;
    expected_ftrace_events.EnableEventsFrom(config.event_filter);
    UnionInPlace(config.atrace_apps, &expected_apps);
    UnionInPlace(config.atrace_categories, &expected_categories);
    UnionInPlace(config.atrace_categories_sdk_optout,
                 &expected_categories_sdk_optout);
  }
  std::vector<std::string> expected_categories_prefer_sdk =
      Subtract(expected_categories, expected_categories_sdk_optout);

  // At this point expected_{apps,categories} contains the union of the
  // leftover configs (if any) that should be still on. However we did not
  // necessarily succeed in turning on atrace for each of those configs
  // previously so we now intersect the {apps,categories} that we *did* manage
  // to turn on with those we want on to determine the new state we should aim
  // for:
  IntersectInPlace(current_state_.atrace_apps, &expected_apps);
  IntersectInPlace(current_state_.atrace_categories, &expected_categories);

  // Work out if there is any difference between the current state and the
  // desired state: It's sufficient to compare sizes here (since we know from
  // above that expected_{apps,categories} is now a subset of
  // atrace_{apps,categories}:
  bool atrace_changed =
      (current_state_.atrace_apps.size() != expected_apps.size()) ||
      (current_state_.atrace_categories.size() != expected_categories.size());

  bool atrace_prefer_sdk_changed =
      current_state_.atrace_categories_prefer_sdk !=
      expected_categories_prefer_sdk;

  if (!SetSyscallEventFilter(/*extra_syscalls=*/{})) {
    PERFETTO_ELOG("Failed to set raw_syscall ftrace filter in RemoveConfig");
  }

  // Disable any events that are currently enabled, but are not in any configs
  // anymore.
  std::set<size_t> event_ids = current_state_.ftrace_events.GetEnabledEvents();
  for (size_t id : event_ids) {
    if (expected_ftrace_events.IsEventEnabled(id))
      continue;
    const Event* event = table_->GetEventById(id);
    // Any event that was enabled must exist.
    PERFETTO_DCHECK(event);
    if (ftrace_->DisableEvent(event->group, event->name))
      current_state_.ftrace_events.DisableEvent(event->ftrace_event_id);

    if (event->group == kKprobeGroup || event->group == kKretprobeGroup) {
      ftrace_->RemoveKprobeEvent(event->group, event->name);
      table_->RemoveEvent({event->group, event->name});
    }
  }

  auto active_it = active_configs_.find(config_id);
  if (active_it != active_configs_.end()) {
    active_configs_.erase(active_it);
    if (active_configs_.empty()) {
      // This was the last active config for now, but potentially more dormant
      // configs need to be activated. We are not interested in reading while no
      // active configs so diasble tracing_on here.
      ftrace_->SetTracingOn(false);
    }
  }

  // Update buffer_percent to the minimum of the remaining configs.
  UpdateBufferPercent();

  // Even if we don't have any other active configs, we might still have idle
  // configs around. Tear down the rest of the ftrace config only if all
  // configs are removed.
  if (ds_configs_.empty()) {
    if (ftrace_->SetCpuBufferSizeInPages(1))
      current_state_.cpu_buffer_size_pages = 1;
    ftrace_->SetBufferPercent(50);
    ftrace_->DisableAllEvents();
    ftrace_->ClearTrace();
    ftrace_->SetTracingOn(current_state_.saved_tracing_on);
  }

  if (current_state_.atrace_on) {
    if (expected_apps.empty() && expected_categories.empty()) {
      DisableAtrace();
    } else if (atrace_changed) {
      // Update atrace to remove the no longer wanted categories/apps. For
      // some categories this won't disable them (e.g. categories that just
      // enable ftrace events) for those there is nothing we can do till the
      // last ftrace config is removed.
      if (StartAtrace(expected_apps, expected_categories,
                      /*atrace_errors=*/nullptr)) {
        // Update current_state_ to reflect this change.
        current_state_.atrace_apps = expected_apps;
        current_state_.atrace_categories = expected_categories;
      }
    }
  }

  if (atrace_prefer_sdk_changed) {
    if (SetAtracePreferSdk(expected_categories_prefer_sdk,
                           /*atrace_errors=*/nullptr)) {
      current_state_.atrace_categories_prefer_sdk =
          expected_categories_prefer_sdk;
    }
  }

  return true;
}

bool FtraceConfigMuxer::ResetCurrentTracer() {
  if (!current_state_.funcgraph_on)
    return true;
  if (!ftrace_->ResetCurrentTracer()) {
    PERFETTO_PLOG("Failed to reset current_tracer to nop");
    return false;
  }
  current_state_.funcgraph_on = false;
  if (!ftrace_->ClearFunctionFilters()) {
    PERFETTO_PLOG("Failed to reset set_ftrace_filter.");
    return false;
  }
  if (!ftrace_->ClearFunctionGraphFilters()) {
    PERFETTO_PLOG("Failed to reset set_function_graph.");
    return false;
  }
  return true;
}

const FtraceDataSourceConfig* FtraceConfigMuxer::GetDataSourceConfig(
    FtraceConfigId id) {
  if (!ds_configs_.count(id))
    return nullptr;
  return &ds_configs_.at(id);
}

void FtraceConfigMuxer::SetupClock(const FtraceConfig& config) {
  std::string current_clock = ftrace_->GetClock();
  std::set<std::string> clocks = ftrace_->AvailableClocks();

  if (config.has_use_monotonic_raw_clock() &&
      config.use_monotonic_raw_clock() && clocks.count(kClockMonoRaw)) {
    ftrace_->SetClock(kClockMonoRaw);
    current_clock = kClockMonoRaw;
  } else {
    for (size_t i = 0; i < base::ArraySize(kClocks); i++) {
      std::string clock = std::string(kClocks[i]);
      if (!clocks.count(clock))
        continue;
      if (current_clock == clock)
        break;
      ftrace_->SetClock(clock);
      current_clock = clock;
      break;
    }
  }

  namespace pb0 = protos::pbzero;
  if (current_clock == "boot") {
    // "boot" is the default expectation on modern kernels, which is why we
    // don't have an explicit FTRACE_CLOCK_BOOT enum and leave it unset.
    // See comments in ftrace_event_bundle.proto.
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_UNSPECIFIED;
  } else if (current_clock == "global") {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_GLOBAL;
  } else if (current_clock == "local") {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_LOCAL;
  } else if (current_clock == kClockMonoRaw) {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_MONO_RAW;
  } else {
    current_state_.ftrace_clock = pb0::FTRACE_CLOCK_UNKNOWN;
  }
}

void FtraceConfigMuxer::SetupBufferSize(const FtraceConfig& request) {
  int64_t phys_ram_pages = sysconf(_SC_PHYS_PAGES);
  size_t pages = ComputeCpuBufferSizeInPages(request.buffer_size_kb(),
                                             request.buffer_size_lower_bound(),
                                             phys_ram_pages);
  ftrace_->SetCpuBufferSizeInPages(pages);
  current_state_.cpu_buffer_size_pages = pages;
}

// Post-conditions:
// * result >= 1 (should have at least one page per CPU)
// * If input is 0 output is a good default number
size_t ComputeCpuBufferSizeInPages(size_t requested_buffer_size_kb,
                                   bool buffer_size_lower_bound,
                                   int64_t sysconf_phys_pages) {
  uint32_t page_sz = base::GetSysPageSize();
  uint64_t default_size_kb =
      (sysconf_phys_pages > 0 &&
       (static_cast<uint64_t>(sysconf_phys_pages) >= (kHighMemBytes / page_sz)))
          ? kDefaultHighRamPerCpuBufferSizeKb
          : kDefaultLowRamPerCpuBufferSizeKb;

  size_t actual_size_kb = requested_buffer_size_kb;
  if ((requested_buffer_size_kb == 0) ||
      (buffer_size_lower_bound && default_size_kb > requested_buffer_size_kb)) {
    actual_size_kb = default_size_kb;
  }

  size_t pages = actual_size_kb / (page_sz / 1024);
  return pages ? pages : 1;
}

// TODO(rsavitski): stop caching the "input" value, as the kernel can and will
// choose a slightly different buffer size (especially on 6.x kernels). And even
// then the value might not be exactly page accurate due to scratch pages (more
// of a concern for the |FtraceController::FlushForInstance| caller).
size_t FtraceConfigMuxer::GetPerCpuBufferSizePages() {
  return current_state_.cpu_buffer_size_pages;
}

// If new_cfg_id is set, consider it in addition to already active configs
// as we're trying to activate it.
bool FtraceConfigMuxer::UpdateBufferPercent() {
  uint32_t kUnsetPercent = std::numeric_limits<uint32_t>::max();
  uint32_t min_percent = kUnsetPercent;
  for (auto cfg_id : active_configs_) {
    auto ds_it = ds_configs_.find(cfg_id);
    if (ds_it != ds_configs_.end() && ds_it->second.buffer_percent > 0) {
      min_percent = std::min(min_percent, ds_it->second.buffer_percent);
    }
  }
  if (min_percent == kUnsetPercent)
    return true;
  // Let the kernel ignore values >100.
  return ftrace_->SetBufferPercent(min_percent);
}

void FtraceConfigMuxer::UpdateAtrace(const FtraceConfig& request,
                                     std::string* atrace_errors) {
  // We want to avoid poisoning current_state_.atrace_{categories, apps}
  // if for some reason these args make atrace unhappy so we stash the
  // union into temps and only update current_state_ if we successfully
  // run atrace.

  std::vector<std::string> combined_categories = request.atrace_categories();
  UnionInPlace(current_state_.atrace_categories, &combined_categories);

  std::vector<std::string> combined_apps = request.atrace_apps();
  UnionInPlace(current_state_.atrace_apps, &combined_apps);

  // Each data source can list some atrace categories for which the SDK is
  // preferred (the rest of the categories are considered to opt out of the
  // SDK). When merging multiple data sources, opting out wins. Therefore this
  // code does a union of the opt outs for all data sources.
  std::vector<std::string> combined_categories_sdk_optout = Subtract(
      request.atrace_categories(), request.atrace_categories_prefer_sdk());

  std::vector<std::string> current_categories_sdk_optout =
      Subtract(current_state_.atrace_categories,
               current_state_.atrace_categories_prefer_sdk);
  UnionInPlace(current_categories_sdk_optout, &combined_categories_sdk_optout);

  std::vector<std::string> combined_categories_prefer_sdk =
      Subtract(combined_categories, combined_categories_sdk_optout);

  if (combined_categories_prefer_sdk !=
      current_state_.atrace_categories_prefer_sdk) {
    if (SetAtracePreferSdk(combined_categories_prefer_sdk, atrace_errors)) {
      current_state_.atrace_categories_prefer_sdk =
          combined_categories_prefer_sdk;
    }
  }

  if (!current_state_.atrace_on ||
      combined_apps.size() != current_state_.atrace_apps.size() ||
      combined_categories.size() != current_state_.atrace_categories.size()) {
    if (StartAtrace(combined_apps, combined_categories, atrace_errors)) {
      current_state_.atrace_categories = combined_categories;
      current_state_.atrace_apps = combined_apps;
      current_state_.atrace_on = true;
    }
  }
}

bool FtraceConfigMuxer::StartAtrace(const std::vector<std::string>& apps,
                                    const std::vector<std::string>& categories,
                                    std::string* atrace_errors) {
  PERFETTO_DLOG("Update atrace config...");

  std::vector<std::string> args;
  args.push_back("atrace");  // argv0 for exec()
  args.push_back("--async_start");
  if (atrace_wrapper_->SupportsUserspaceOnly())
    args.push_back("--only_userspace");

  for (const auto& category : categories)
    args.push_back(category);

  if (!apps.empty()) {
    args.push_back("-a");
    std::string arg = "";
    for (const auto& app : apps) {
      arg += app;
      arg += ",";
    }
    arg.resize(arg.size() - 1);
    args.push_back(arg);
  }

  bool result = atrace_wrapper_->RunAtrace(args, atrace_errors);
  PERFETTO_DLOG("...done (%s)", result ? "success" : "fail");
  return result;
}

bool FtraceConfigMuxer::SetAtracePreferSdk(
    const std::vector<std::string>& prefer_sdk_categories,
    std::string* atrace_errors) {
  if (!atrace_wrapper_->SupportsPreferSdk()) {
    return false;
  }
  PERFETTO_DLOG("Update atrace prefer sdk categories...");

  std::vector<std::string> args;
  args.push_back("atrace");  // argv0 for exec()
  args.push_back("--prefer_sdk");

  for (const auto& category : prefer_sdk_categories)
    args.push_back(category);

  bool result = atrace_wrapper_->RunAtrace(args, atrace_errors);
  PERFETTO_DLOG("...done (%s)", result ? "success" : "fail");
  return result;
}

void FtraceConfigMuxer::DisableAtrace() {
  PERFETTO_DCHECK(current_state_.atrace_on);

  PERFETTO_DLOG("Stop atrace...");

  std::vector<std::string> args{"atrace", "--async_stop"};
  if (atrace_wrapper_->SupportsUserspaceOnly())
    args.push_back("--only_userspace");
  if (atrace_wrapper_->RunAtrace(args, /*atrace_errors=*/nullptr)) {
    current_state_.atrace_categories.clear();
    current_state_.atrace_apps.clear();
    current_state_.atrace_on = false;
  }

  PERFETTO_DLOG("...done");
}

}  // namespace perfetto
