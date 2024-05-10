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

#include "src/trace_redaction/process_thread_timeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "perfetto/base/logging.h"

namespace perfetto::trace_redaction {
namespace {
// Limit the number of iterations to avoid an infinite loop. 10 is a generous
// number of iterations.
constexpr size_t kMaxSearchDepth = 10;

bool OrderByPid(const ProcessThreadTimeline::Event& left,
                const ProcessThreadTimeline::Event& right) {
  return left.pid < right.pid;
}

}  // namespace

void ProcessThreadTimeline::Append(const Event& event) {
  events_.push_back(event);
  mode_ = Mode::kWrite;
}

void ProcessThreadTimeline::Sort() {
  std::sort(events_.begin(), events_.end(), OrderByPid);
  mode_ = Mode::kRead;
}

bool ProcessThreadTimeline::PidConnectsToUid(uint64_t ts,
                                             int32_t pid,
                                             uint64_t uid) const {
  PERFETTO_DCHECK(mode_ == Mode::kRead);

  auto event = FindPreviousEvent(ts, pid);

  for (size_t d = 0; d < kMaxSearchDepth; ++d) {
    // The thread/process was freed. It won't exist until a new open event.
    if (event.type != Event::Type::kOpen) {
      return false;
    }

    // TODO(vaage): Normalize the uid values.
    if (event.uid == uid) {
      return true;
    }

    // If there is no parent, there is no way to keep searching.
    if (event.ppid == Event::kUnknownPid) {
      return false;
    }

    event = FindPreviousEvent(ts, event.ppid);
  }

  return false;
}

ProcessThreadTimeline::Event ProcessThreadTimeline::FindPreviousEvent(
    uint64_t ts,
    int32_t pid) const {
  PERFETTO_DCHECK(mode_ == Mode::kRead);

  Event fake = Event::Close(ts, pid);

  // Events are sorted by pid, creating islands of data. This search is to put
  // the cursor at the start of pid's island. Each island will be small (a
  // couple of items), so searching within the islands should be cheap.
  auto at = std::lower_bound(events_.begin(), events_.end(), fake, OrderByPid);

  // `pid` was not found in `events_`.
  if (at == events_.end()) {
    return {};
  }

  // "no best option".
  Event best = {};

  // Run through all events (related to this pid) and find the last event that
  // comes before ts. If the events were in order by time, the search could be
  // more efficient, but the gains are margin because:
  //
  // 1. The number of edge cases go up.
  //
  // 2. The code is harder to read.
  //
  // 3. The performance gains are minimal or non-existant because of the small
  //    number of events.
  for (; at != events_.end() && at->pid == pid; ++at) {
    // This event is after "now" and can safely be ignored.
    if (at->ts > ts) {
      continue;
    }

    // `at` is know to be before now. So it is always safe to accept an event.
    //
    // All ts values are positive. However, ts_at and ts_best are both less than
    // ts (see early condition), meaning they can be considered negative values.
    //
    //      at        best            ts
    //   <---+-----------+-------------+---->
    //      31          64            93
    //
    //      at        best            ts
    //   <---+-----------+-------------+---->
    //     -62         -29             0
    //
    // This means that the latest ts value under ts is the closest to ts.

    if (best.type == Event::Type::kInvalid || at->ts > best.ts) {
      best = *at;
    }

    // This handles the rare edge case where an open and close event occur at
    // the same time. The close event must get priority. This is done by
    // allowing close events to use ">=" where as other events can only use ">".
    if (at->type == Event::Type::kClose && at->ts == best.ts) {
      best = *at;
    }
  }

  return best;
}

}  // namespace perfetto::trace_redaction
