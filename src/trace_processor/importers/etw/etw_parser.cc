/*
 etw* Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/etw/etw_parser.h"

#include <cstdint>
#include <limits>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/thread_state_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/trace/etw/etw.pbzero.h"
#include "protos/perfetto/trace/etw/etw_event.pbzero.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {

using protozero::ConstBytes;

}  // namespace

EtwParser::EtwParser(TraceProcessorContext* context) : context_(context) {}

base::Status EtwParser::ParseEtwEvent(uint32_t cpu,
                                      int64_t ts,
                                      const TracePacketData& data) {
  using protos::pbzero::EtwTraceEvent;
  const TraceBlobView& event = data.packet;
  protos::pbzero::EtwTraceEvent::Decoder decoder(event.data(), event.length());

  if (decoder.has_c_switch()) {
    ParseCswitch(ts, cpu, decoder.c_switch());
  }

  if (decoder.has_ready_thread()) {
    ParseReadyThread(ts, decoder.thread_id(), decoder.ready_thread());
  }

  if (decoder.has_mem_info()) {
    ParseMemInfo(ts, decoder.mem_info());
  }

  return base::OkStatus();
}

void EtwParser::ParseCswitch(int64_t timestamp, uint32_t cpu, ConstBytes blob) {
  protos::pbzero::CSwitchEtwEvent::Decoder cs(blob);
  int32_t old_thread_state = cs.has_old_thread_state()
                                 ? cs.old_thread_state()
                                 : cs.old_thread_state_int();
  // thread_id might be erased for privacy/security concerns, in this case, use
  // a dummy id since 0 means idle.
  uint32_t old_thread_id =
      cs.has_old_thread_id() ? cs.old_thread_id() : uint32_t(-1);
  uint32_t new_thread_id =
      cs.has_new_thread_id() ? cs.new_thread_id() : uint32_t(-1);
  PushSchedSwitch(cpu, timestamp, old_thread_id, old_thread_state,
                  new_thread_id, cs.new_thread_priority());
}

void EtwParser::ParseReadyThread(int64_t timestamp,
                                 uint32_t waker_tid,
                                 ConstBytes blob) {
  protos::pbzero::ReadyThreadEtwEvent::Decoder rt(blob);
  UniqueTid wakee_utid =
      context_->process_tracker->GetOrCreateThread(rt.t_thread_id());
  UniqueTid waker_utid =
      context_->process_tracker->GetOrCreateThread(waker_tid);
  ThreadStateTracker::GetOrCreate(context_)->PushWakingEvent(
      timestamp, wakee_utid, waker_utid);
}

void EtwParser::ParseMemInfo(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::MemInfoEtwEvent::Decoder meminfo(blob);
  static constexpr auto kEtwMeminfoBlueprint = tracks::CounterBlueprint(
      "etw_meminfo", tracks::StaticUnitBlueprint("pages"),
      tracks::DimensionBlueprints(
          tracks::StringDimensionBlueprint("counter_type")),
      tracks::FnNameBlueprint([](base::StringView type) {
        return base::StackString<255>("%.*s Page Count", int(type.size()),
                                      type.data());
      }));

  TrackId zero_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("Zero"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.zero_page_count()),
      zero_page_count_track_id);

  TrackId free_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("Free"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.free_page_count()),
      free_page_count_track_id);

  TrackId modified_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("Modified"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.modified_page_count()),
      modified_page_count_track_id);

  TrackId modified_no_write_page_count_track_id =
      context_->track_tracker->InternTrack(
          kEtwMeminfoBlueprint, tracks::Dimensions("ModifiedNoWrite"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.modified_no_write_page_count()),
      modified_no_write_page_count_track_id);

  TrackId bad_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("Bad"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.bad_page_count()),
      bad_page_count_track_id);

  TrackId modified_page_count_page_file_track_id =
      context_->track_tracker->InternTrack(
          kEtwMeminfoBlueprint, tracks::Dimensions("ModifiedPageFile"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.modified_page_count_page_file()),
      modified_page_count_page_file_track_id);

  TrackId paged_pool_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("PagedPool"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.paged_pool_page_count()),
      paged_pool_page_count_track_id);

  TrackId non_paged_pool_page_count_track_id =
      context_->track_tracker->InternTrack(kEtwMeminfoBlueprint,
                                           tracks::Dimensions("NonPagedPool"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.non_paged_pool_page_count()),
      non_paged_pool_page_count_track_id);

  TrackId mdl_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("Mdl"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.mdl_page_count()),
      mdl_page_count_track_id);

  TrackId commit_page_count_track_id = context_->track_tracker->InternTrack(
      kEtwMeminfoBlueprint, tracks::Dimensions("Commit"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(meminfo.commit_page_count()),
      commit_page_count_track_id);

  auto standby_page_count_iterator = meminfo.standby_page_counts();
  for (int i = 0; standby_page_count_iterator;
       ++i, ++standby_page_count_iterator) {
    const std::string name = "Standby Pri-" + std::to_string(i);
    TrackId standby_page_count_track_id = context_->track_tracker->InternTrack(
        kEtwMeminfoBlueprint, tracks::Dimensions(base::StringView(name)));
    context_->event_tracker->PushCounter(
        timestamp, static_cast<double>(*standby_page_count_iterator),
        standby_page_count_track_id);
  }

  auto repurposed_page_count_iterator = meminfo.repurposed_page_counts();
  for (int i = 0; repurposed_page_count_iterator;
       ++i, ++repurposed_page_count_iterator) {
    const std::string name = "Repurposed Pri-" + std::to_string(i);
    TrackId repurposed_page_count_track_id =
        context_->track_tracker->InternTrack(
            kEtwMeminfoBlueprint, tracks::Dimensions(base::StringView(name)));
    context_->event_tracker->PushCounter(
        timestamp, static_cast<double>(*repurposed_page_count_iterator),
        repurposed_page_count_track_id);
  }
}

void EtwParser::PushSchedSwitch(uint32_t cpu,
                                int64_t ts,
                                uint32_t prev_tid,
                                int32_t prev_state,
                                uint32_t next_tid,
                                int32_t next_prio) {
  UniqueTid next_utid = context_->process_tracker->GetOrCreateThread(next_tid);

  // First use this data to close the previous slice.
  bool prev_pid_match_prev_next_pid = false;
  auto* pending_sched = sched_event_state_.GetPendingSchedInfoForCpu(cpu);
  uint32_t pending_slice_idx = pending_sched->pending_slice_storage_idx;
  StringId prev_state_string_id = TaskStateToStringId(prev_state);
  if (prev_state_string_id == kNullStringId) {
    context_->storage->IncrementStats(stats::task_state_invalid);
  }
  if (pending_slice_idx < std::numeric_limits<uint32_t>::max()) {
    prev_pid_match_prev_next_pid = prev_tid == pending_sched->last_pid;
    if (PERFETTO_LIKELY(prev_pid_match_prev_next_pid)) {
      context_->sched_event_tracker->ClosePendingSlice(pending_slice_idx, ts,
                                                       prev_state_string_id);
    } else {
      // If the pids are not consistent, make a note of this.
      context_->storage->IncrementStats(stats::mismatched_sched_switch_tids);
    }
  }

  auto new_slice_idx = context_->sched_event_tracker->AddStartSlice(
      cpu, ts, next_utid, next_prio);

  // Finally, update the info for the next sched switch on this CPU.
  pending_sched->pending_slice_storage_idx = new_slice_idx;
  pending_sched->last_pid = next_tid;
  pending_sched->last_utid = next_utid;
  pending_sched->last_prio = next_prio;

  UniqueTid prev_utid = context_->process_tracker->GetOrCreateThread(prev_tid);

  // Update the ThreadState table.
  ThreadStateTracker::GetOrCreate(context_)->PushSchedSwitchEvent(
      ts, cpu, prev_utid, prev_state_string_id, next_utid);
}

StringId EtwParser::TaskStateToStringId(int64_t task_state_int) {
  const auto state = static_cast<uint8_t>(task_state_int);

  // Mapping for the different Etw states with their string description.
  static constexpr std::string_view etw_states[] = {
      "Initialized",    // 0x00
      "Ready",          // 0x01
      "Running",        // 0x02
      "Standby",        // 0x03
      "Terminated",     // 0x04
      "Waiting",        // 0x05
      "Transition",     // 0x06
      "DeferredReady",  // 0x07
  };

  if (state >= std::size(etw_states)) {
    return kNullStringId;
  }
  return context_->storage->InternString(etw_states[state]);
}

}  // namespace trace_processor
}  // namespace perfetto
