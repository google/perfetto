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

#ifndef SRC_TRACE_REDACTION_COLLECT_TIMELINE_EVENTS_H_
#define SRC_TRACE_REDACTION_COLLECT_TIMELINE_EVENTS_H_

#include <cstdint>
#include <optional>

#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

// Creates events from process_tree, task_newtask, and sched_process_free
// packets and stores them in a timeline.
class CollectTimelineEvents : public CollectPrimitive {
 public:
  // When set, only packets whose trusted_uid equals `uid` contribute to the
  // timeline (e.g. Context::kTracedProbesUid, so packets written by apps can
  // never change which pids belong to the target).
  void set_timeline_writer_uid(int32_t uid) { timeline_writer_uid_ = uid; }

  // When true, the first open event of every pid is moved back to the
  // process's start time (ProcessTree.Process.process_start_from_boot) or to
  // 0 if unknown. See ProcessThreadTimeline::BackdateFirstOpenEvents().
  void set_backdate_first_open(bool backdate) {
    backdate_first_open_ = backdate;
  }

  base::Status Begin(Context* context) const override;

  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       Context* context) const override;

  base::Status End(Context* context) const override;

 private:
  std::optional<int32_t> timeline_writer_uid_;
  bool backdate_first_open_ = false;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_COLLECT_TIMELINE_EVENTS_H_
