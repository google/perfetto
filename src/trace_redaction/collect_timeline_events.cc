/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/collect_timeline_events.h"

#include "perfetto/ext/base/status_macros.h"
#include "src/trace_redaction/process_thread_timeline.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;
using ProcessTree = protos::pbzero::ProcessTree;
using FtraceEvent = protos::pbzero::FtraceEvent;
using FtraceEventBundle = protos::pbzero::FtraceEventBundle;
using SchedProcessFreeFtraceEvent = protos::pbzero::SchedProcessFreeFtraceEvent;
using TaskNewtaskFtraceEvent = protos::pbzero::TaskNewtaskFtraceEvent;

void MarkOpen(uint64_t ts,
              const ProcessTree::Process::Decoder& process,
              bool normalize_uid,
              ProcessThreadTimeline* timeline) {
  if (process.has_uid() && process.uid() >= 0) {
    auto uid = static_cast<uint64_t>(process.uid());
    // ProcessTree records full per-user Linux UIDs (e.g. 1010234 for User 10),
    // whereas PackagesList only records normalized UIDs (e.g. 10234). When an
    // explicit target UID is provided (--uid), store the full 64-bit UID so
    // the timeline strictly isolates User 0 from User 10. When no target UID is
    // provided (normalize_uid is true), normalize ProcessTree UIDs via
    // NormalizeUid() so secondary profile processes match the normalized UID
    // resolved from PackagesList by FindPackageUid.
    if (normalize_uid) {
      uid = NormalizeUid(uid);
    }
    auto e = ProcessThreadTimeline::Event::Open(ts, process.pid(),
                                                process.ppid(), uid);
    timeline->Append(e);
  } else {
    auto e =
        ProcessThreadTimeline::Event::Open(ts, process.pid(), process.ppid());
    timeline->Append(e);
  }
}

void MarkOpen(uint64_t ts,
              const ProcessTree::Thread::Decoder& thread,
              ProcessThreadTimeline* timeline) {
  if (thread.tid() == thread.tgid()) {
    // This is the main thread of a process, we can ignore it given this thread
    // can't be used to reach the root parent and the process entry which can
    // perform that task already exists in the timeline. Furthermore, keeping
    // this entry would cause a bug where the timeline discards every packet as
    // threads do not link to a UID, they need to go up to an actual process for
    // that test, so they always fail and having the same parent causes an
    // infinite loop that kicks the built-in safeguard of 10 max parent lookup,
    // wasting resources and causing entire processes or threads to be dropped
    // in the final redacted trace.
    return;
  }
  auto e = ProcessThreadTimeline::Event::Open(ts, thread.tid(), thread.tgid());
  timeline->Append(e);
}

void MarkClose(const FtraceEvent::Decoder& event,
               const SchedProcessFreeFtraceEvent::Decoder process_free,
               ProcessThreadTimeline* timeline) {
  auto e = ProcessThreadTimeline::Event::Close(event.timestamp(),
                                               process_free.pid());
  timeline->Append(e);
}

void MarkOpen(const FtraceEvent::Decoder& event,
              const TaskNewtaskFtraceEvent::Decoder new_task,
              ProcessThreadTimeline* timeline) {
  // Event though pid() is uint32_t. all other pid values use int32_t, so it's
  // assumed to be safe to narrow-cast it.
  auto ppid = static_cast<int32_t>(event.pid());
  auto e = ProcessThreadTimeline::Event::Open(event.timestamp(), new_task.pid(),
                                              ppid);
  timeline->Append(e);
}

void AppendEvents(uint64_t ts,
                  const ProcessTree::Decoder& tree,
                  bool normalize_uid,
                  ProcessThreadTimeline* timeline) {
  for (auto it = tree.processes(); it; ++it) {
    MarkOpen(ts, ProcessTree::Process::Decoder(*it), normalize_uid, timeline);
  }

  for (auto it = tree.threads(); it; ++it) {
    MarkOpen(ts, ProcessTree::Thread::Decoder(*it), timeline);
  }
}

void AppendEvents(const FtraceEventBundle::Decoder& ftrace_events,
                  ProcessThreadTimeline* timeline) {
  for (auto it = ftrace_events.event(); it; ++it) {
    FtraceEvent::Decoder event(*it);

    if (event.has_task_newtask()) {
      MarkOpen(event, TaskNewtaskFtraceEvent::Decoder(event.task_newtask()),
               timeline);
      continue;
    }

    if (event.has_sched_process_free()) {
      MarkClose(
          event,
          SchedProcessFreeFtraceEvent::Decoder(event.sched_process_free()),
          timeline);
      continue;
    }
  }
}

void RecordProcessStarts(const TracePacket::Decoder& packet,
                         Context::TimelineBackdating* backdating) {
  if (!packet.has_process_tree()) {
    return;
  }
  uint64_t ts = packet.timestamp();
  ProcessTree::Decoder tree(packet.process_tree());
  for (auto it = tree.processes(); it; ++it) {
    ProcessTree::Process::Decoder process(*it);
    if (!process.has_pid() || !process.has_process_start_from_boot()) {
      continue;
    }
    auto [entry, inserted] = backdating->first_snapshot_start.emplace(
        process.pid(), std::make_pair(ts, process.process_start_from_boot()));
    if (!inserted && ts < entry->second.first) {
      entry->second = {ts, process.process_start_from_boot()};
    }
  }
}

}  // namespace

base::Status CollectTimelineEvents::Begin(Context* context) const {
  // This primitive is artificially limited to owning the timeline. In practice
  // there is no reason why multiple primitives could contribute to the
  // timeline.
  if (context->timeline) {
    return base::ErrStatus(
        "CollectTimelineEvents: timeline was already initialized");
  }

  context->timeline = std::make_unique<ProcessThreadTimeline>();
  return base::OkStatus();
}

base::Status CollectTimelineEvents::Collect(const TracePacket::Decoder& packet,
                                            Context* context) const {
  if (backdate_first_open_) {
    RecordProcessStarts(packet, &context->timeline_backdating);
  }

  // Unlike ftrace events, process trees do not provide per-process or
  // per-thread timing information. The packet has timestamp and the process
  // tree has collection_end_timestamp (collection_end_timestamp > timestamp).
  //
  // The packet's timestamp based on the assumption that in order to be
  // collected, the processes and threads had to exist before "now".
  if (packet.has_process_tree()) {
    context->process_tree_packets++;
    AppendEvents(packet.timestamp(),
                 ProcessTree::Decoder(packet.process_tree()),
                 context->normalize_uid, context->timeline.get());
  }

  if (packet.has_ftrace_events()) {
    AppendEvents(FtraceEventBundle::Decoder(packet.ftrace_events()),
                 context->timeline.get());
  }

  return base::OkStatus();
}

base::Status CollectTimelineEvents::End(Context* context) const {
  if (backdate_first_open_) {
    // Process starts are CLOCK_BOOTTIME (`starttime` in /proc/pid/stat); they
    // are only comparable with packet timestamps when the trace clock is also
    // BOOTTIME (populated on `clock_converter` by CollectClocks, defaulting to
    // BOOTTIME).
    ASSIGN_OR_RETURN(ClockId trace_clock,
                     context->clock_converter.GetTraceClock());
    bool is_boottime =
        trace_clock == ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME);

    auto& backdating = context->timeline_backdating;
    std::unordered_map<int32_t, uint64_t> start_ts_by_pid;
    if (is_boottime) {
      for (const auto& [pid, snapshot_and_start] :
           backdating.first_snapshot_start) {
        start_ts_by_pid[pid] = snapshot_and_start.second;
      }
    }
    bool has_ftrace = context->system_info.has_value() &&
                      context->system_info->cpu_count() > 0;
    context->timeline->BackdateFirstOpenEvents(
        start_ts_by_pid,
        /*fallback_to_zero=*/!has_ftrace);
    backdating = Context::TimelineBackdating();
  }

  // Sort must be called in order to read from the timeline. If any more events
  // are added after this, then sort will need to be called again.
  context->timeline->Sort();
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
