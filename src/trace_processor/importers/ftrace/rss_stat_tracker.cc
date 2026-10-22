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

#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/kmem.pbzero.h"
#include "protos/perfetto/trace/ftrace/synthetic.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using FtraceEvent = protos::pbzero::FtraceEvent;

const char* GetProcessMemoryKey(uint32_t member) {
  switch (member) {
    case 0:
      return "rss.file";
    case 1:
      return "rss.anon";
    case 2:
      return "swap";
    case 3:
      return "rss.shmem";
    case 4:
      return "unreclaimable";
    default:
      return "unknown";
  }
}

}  // namespace

RssStatTracker::RssStatTracker(TraceProcessorContext* context)
    : context_(context) {}

void RssStatTracker::ParseRssStat(int64_t ts,
                                  uint32_t field_id,
                                  uint32_t pid,
                                  ConstBytes blob) {
  uint32_t member;
  int64_t size;
  std::optional<bool> curr;
  std::optional<int64_t> mm_id;

  if (field_id == FtraceEvent::kRssStatFieldNumber) {
    protos::pbzero::RssStatFtraceEvent::Decoder rss(blob);

    member = static_cast<uint32_t>(rss.member());
    size = rss.size();
    if (rss.has_curr()) {
      curr = std::make_optional(static_cast<bool>(rss.curr()));
    }
    if (rss.has_mm_id()) {
      mm_id = std::make_optional(rss.mm_id());
    }

    ParseRssStat(ts, pid, size, member, curr, mm_id, /*throttled=*/false);
  } else if (field_id == FtraceEvent::kRssStatThrottledFieldNumber) {
    protos::pbzero::RssStatThrottledFtraceEvent::Decoder rss(blob);

    member = static_cast<uint32_t>(rss.member());
    size = rss.size();
    curr = std::make_optional(static_cast<bool>(rss.curr()));
    mm_id = std::make_optional(rss.mm_id());

    ParseRssStat(ts, pid, size, member, curr, mm_id, /*throttled=*/true);
  } else {
    PERFETTO_DFATAL("Unexpected field id");
  }
}

void RssStatTracker::ParseRssStat(int64_t ts,
                                  uint32_t pid,
                                  int64_t size,
                                  uint32_t member,
                                  std::optional<bool> curr,
                                  std::optional<int64_t> mm_id,
                                  bool throttled) {
  const char* memory_key = GetProcessMemoryKey(member);
  if (!memory_key) {
    context_->stats_tracker->IncrementStats(stats::rss_stat_unknown_keys);
    return;
  }
  if (size < 0) {
    context_->stats_tracker->IncrementStats(stats::rss_stat_negative_size);
    return;
  }

  std::optional<UniqueTid> utid;
  if (mm_id.has_value() && curr.has_value()) {
    utid = FindUtidForMmId(*mm_id, *curr, pid, throttled);
  } else {
    utid = context_->process_tracker->GetOrCreateThread(pid);
  }

  if (utid) {
    context_->event_tracker->PushProcessCounterForThread(
        EventTracker::RssStat{memory_key}, ts, static_cast<double>(size),
        *utid);
  } else {
    context_->stats_tracker->IncrementStats(
        stats::rss_stat_unknown_thread_for_mm_id);
  }
}

std::optional<UniqueTid> RssStatTracker::FindUtidForMmId(int64_t mm_id,
                                                         bool is_curr,
                                                         uint32_t pid,
                                                         bool throttled) {
  // If curr is true, we can usually just overwrite the state in the map and
  // return the utid correspodning to |pid|.
  if (is_curr) {
    UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
    auto [owner_utid, inserted] = mm_id_to_utid_.Insert(mm_id, utid);
    if (throttled && !inserted && *owner_utid != utid &&
        IsRacyCurrEvent(mm_id, *owner_utid, utid)) {
      context_->stats_tracker->IncrementStats(
          stats::rss_stat_curr_ignored_mm_owned_by_other_process);
      return *owner_utid;
    }
    *owner_utid = utid;
    if (throttled) {
      if (utid >= utid_to_curr_mm_id_.size())
        utid_to_curr_mm_id_.resize(utid + 1, kNoMmId);
      utid_to_curr_mm_id_[utid] = mm_id;
    }
    return utid;
  }

  // If curr is false, try and lookup the utid we previously saw for this
  // mm id.
  auto* it = mm_id_to_utid_.Find(mm_id);
  if (!it)
    return std::nullopt;

  // Verify that the utid in the map is still alive. This can happen if an mm
  // struct we saw in the past is about to be reused after thread but we don't
  // know the new process that struct will be associated with.
  const UniqueTid mm_utid = *it;
  if (!context_->process_tracker->IsThreadAlive(mm_utid)) {
    mm_id_to_utid_.Erase(mm_id);
    return std::nullopt;
  }

  // This case happens when a process is changing the VM of another process and
  // we know the utid corresponding to the target process; or when the current
  // thread is tearing down its own mm (e.g. during exit_mm(), where
  // current->mm is cleared before mmput() synchronously emits teardown
  // rss_stat events, or during exec() while the old mm is released after the
  // new mm has been installed). In either case the event describes the
  // recorded owner's mm, so return that utid.
  return mm_utid;
}

bool RssStatTracker::IsRacyCurrEvent(int64_t mm_id,
                                     UniqueTid owner_utid,
                                     UniqueTid utid) {
  // BEST EFFORT heuristic for b/576695708; only applied to rss_stat_throttled
  // as raw rss_stat events are not affected by the race.
  //
  // The hist trigger backing rss_stat_throttled keeps one element per
  // (mm_id, member) shared by all CPUs. The curr/size field variables are
  // written into it and read back to build the synthetic event without any
  // cross-CPU serialization. So a thread touching another process's mm (e.g.
  // direct reclaim, kswapd) can emit curr=true with the values of an event
  // emitted concurrently by the mm's owner. The curr field of these events is
  // therefore not trustworthy and the trace does not, in general, contain
  // enough information to tell such events apart from legitimate ones.
  //
  // We treat curr=true as curr=false when:
  //  1. the recorded owner thread's last curr=true event was for this mm,
  //  2. the emitting thread is in a different process to the owner and
  //  3. the owner is still alive.
  //
  // Note: vfork()/CLONE_VM children use their parent's mm until they exec;
  // their curr=true events for it are (intentionally) attributed to the
  // parent which owns the mm.
  //
  // Known cases where this is wrong:
  //  * mm_id reuse after the owner exits but before the owner is reaped
  //    (sched_process_free can arrive 100s of ms after the mm is released):
  //    the new process' events are attributed to the dead owner until the
  //    owner thread is marked as dead.
  // Known cases this does not catch:
  //  * The racing event is the first curr=true event we see for the mm or
  //    races with a curr=true event which was itself bogus.
  //  * Either thread is not associated with a process.
  //
  // This is only called when the emitting thread differs from the recorded
  // owner; checks are ordered cheapest first. The most common caller is other
  // threads of the same process, which bail out at (2).
  if (owner_utid >= utid_to_curr_mm_id_.size() ||
      utid_to_curr_mm_id_[owner_utid] != mm_id) {
    return false;
  }
  const auto& threads = context_->storage->thread_table();
  std::optional<UniquePid> upid = threads[utid].upid();
  if (!upid)
    return false;
  std::optional<UniquePid> owner_upid = threads[owner_utid].upid();
  if (!owner_upid || *owner_upid == *upid)
    return false;
  return context_->process_tracker->IsThreadAlive(owner_utid);
}

}  // namespace perfetto::trace_processor
