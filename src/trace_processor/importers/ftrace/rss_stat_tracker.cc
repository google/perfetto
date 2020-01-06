/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/rss_stat_tracker.h"

#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "protos/perfetto/trace/ftrace/kmem.pbzero.h"

namespace perfetto {
namespace trace_processor {

RssStatTracker::RssStatTracker(TraceProcessorContext* context)
    : context_(context) {
  rss_members_.emplace_back(context->storage->InternString("mem.rss.file"));
  rss_members_.emplace_back(context->storage->InternString("mem.rss.anon"));
  rss_members_.emplace_back(context->storage->InternString("mem.swap"));
  rss_members_.emplace_back(context->storage->InternString("mem.rss.shmem"));
  rss_members_.emplace_back(
      context->storage->InternString("mem.rss.unknown"));  // Keep this last.
}

void RssStatTracker::ParseRssStat(int64_t ts, uint32_t pid, ConstBytes blob) {
  protos::pbzero::RssStatFtraceEvent::Decoder rss(blob.data, blob.size);
  const auto kRssStatUnknown = static_cast<uint32_t>(rss_members_.size()) - 1;
  auto member = static_cast<uint32_t>(rss.member());
  int64_t size = rss.size();
  if (member >= rss_members_.size()) {
    context_->storage->IncrementStats(stats::rss_stat_unknown_keys);
    member = kRssStatUnknown;
  }

  if (size < 0) {
    context_->storage->IncrementStats(stats::rss_stat_negative_size);
    return;
  }

  base::Optional<UniqueTid> utid;
  if (rss.has_mm_id()) {
    PERFETTO_DCHECK(rss.has_curr());
    utid = FindUtidForMmId(rss.mm_id(), rss.curr(), pid);
  } else {
    utid = context_->process_tracker->GetOrCreateThread(pid);
  }

  if (utid) {
    context_->event_tracker->PushProcessCounterForThread(
        ts, size, rss_members_[member], *utid);
  } else {
    context_->storage->IncrementStats(stats::rss_stat_unknown_thread_for_mm_id);
  }
}

base::Optional<UniqueTid> RssStatTracker::FindUtidForMmId(int64_t mm_id,
                                                          bool is_curr,
                                                          uint32_t pid) {
  auto it = mm_id_to_utid_.find(mm_id);
  if (!is_curr) {
    return it == mm_id_to_utid_.end() ? base::nullopt
                                      : base::make_optional(it->second);
  }

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  if (it != mm_id_to_utid_.end() && it->second != utid) {
    // Since both of these structs have the same mm hash and both say that
    // the mm hash is for the current project, we can assume they belong to
    // the same process so we can associate them together.
    // TODO(lalitm): investigate if it's possible for mm_id to be reused
    // between different processes if we have pid reuse and get unlucky. If
    // so, we'll need to do some more careful tracking here.
    context_->process_tracker->AssociateThreads(it->second, utid);
  }
  mm_id_to_utid_[mm_id] = utid;
  return utid;
}

}  // namespace trace_processor
}  // namespace perfetto
