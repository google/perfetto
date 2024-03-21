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

#include "src/trace_redaction/build_timeline.h"

#include "perfetto/ext/base/status_or.h"
#include "src/trace_redaction/process_thread_timeline.h"
#include "src/trace_redaction/trace_redaction_framework.h"

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
              ProcessTree::Process::Decoder process,
              ProcessThreadTimeline* timeline) {
  // The uid in the process tree is a int32_t, but in the package list, the uid
  // is a uint64_t.
  auto uid = static_cast<uint64_t>(process.uid());
  auto e = ProcessThreadTimeline::Event::Open(ts, process.pid(), process.ppid(),
                                              uid);
  timeline->Append(e);
}

void MarkOpen(uint64_t ts,
              ProcessTree::Thread::Decoder thread,
              ProcessThreadTimeline* timeline) {
  auto e = ProcessThreadTimeline::Event::Open(ts, thread.tid(), thread.tgid());
  timeline->Append(e);
}

void MarkClose(const FtraceEvent::Decoder& event,
               SchedProcessFreeFtraceEvent::Decoder process_free,
               ProcessThreadTimeline* timeline) {
  auto e = ProcessThreadTimeline::Event::Close(event.timestamp(),
                                               process_free.pid());
  timeline->Append(e);
}

void MarkOpen(const FtraceEvent::Decoder& event,
              TaskNewtaskFtraceEvent::Decoder new_task,
              ProcessThreadTimeline* timeline) {
  // Event though pid() is uint32_t. all other pid values use int32_t, so it's
  // assumed to be safe to narrow-cast it.
  auto ppid = static_cast<int32_t>(event.pid());
  auto e = ProcessThreadTimeline::Event::Open(event.timestamp(), new_task.pid(),
                                              ppid);
  timeline->Append(e);
}

void AppendEvents(uint64_t ts,
                  ProcessTree::Decoder tree,
                  ProcessThreadTimeline* timeline) {
  for (auto it = tree.processes(); it; ++it) {
    MarkOpen(ts, ProcessTree::Process::Decoder(*it), timeline);
  }

  for (auto it = tree.threads(); it; ++it) {
    MarkOpen(ts, ProcessTree::Thread::Decoder(*it), timeline);
  }
}

void AppendEvents(FtraceEventBundle::Decoder ftrace_events,
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

}  // namespace

base::StatusOr<CollectPrimitive::ContinueCollection> BuildTimeline::Collect(
    const TracePacket::Decoder& packet,
    Context* context) const {
  // TODO(vaage): This should only be true on the first call. However, that
  // means a branch is called N times when N-1 times it will be false. This may
  // be common across Collect primitives. Having a "begin" and "end" end-points.
  if (!context->timeline) {
    context->timeline = std::make_unique<ProcessThreadTimeline>();
  }

  // Unlike ftrace events, process trees do not provide per-process or
  // per-thread timing information. The packet has timestamp and the process
  // tree has collection_end_timestamp (collection_end_timestamp > timestamp).
  //
  // The packet's timestamp based on the assumption that in order to be
  // collected, the processes and threads had to exist before "now".
  if (packet.has_process_tree()) {
    AppendEvents(packet.timestamp(),
                 ProcessTree::Decoder(packet.process_tree()),
                 context->timeline.get());
    return ContinueCollection::kNextPacket;
  }

  if (packet.has_ftrace_events()) {
    AppendEvents(FtraceEventBundle::Decoder(packet.ftrace_events()),
                 context->timeline.get());
    return ContinueCollection::kNextPacket;
  }

  return ContinueCollection::kNextPacket;
}

}  // namespace perfetto::trace_redaction
