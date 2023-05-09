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

#include "src/trace_processor/importers/systrace/systrace_parser.h"

#include <optional>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

SystraceParser::SystraceParser(TraceProcessorContext* ctx)
    : context_(ctx),
      lmk_id_(ctx->storage->InternString("mem.lmk")),
      oom_score_adj_id_(ctx->storage->InternString("oom_score_adj")),
      screen_state_id_(ctx->storage->InternString("ScreenState")),
      cookie_id_(ctx->storage->InternString("cookie")) {}

SystraceParser::~SystraceParser() = default;

void SystraceParser::ParsePrintEvent(int64_t ts,
                                     uint32_t pid,
                                     base::StringView event) {
  systrace_utils::SystraceTracePoint point{};
  switch (ParseSystraceTracePoint(event, &point)) {
    case systrace_utils::SystraceParseResult::kSuccess:
      ParseSystracePoint(ts, pid, point);
      break;
    case systrace_utils::SystraceParseResult::kFailure:
      context_->storage->IncrementStats(stats::systrace_parse_failure);
      break;
    case systrace_utils::SystraceParseResult::kUnsupported:
      // Silently ignore unsupported results.
      break;
  }
}

void SystraceParser::ParseZeroEvent(int64_t ts,
                                    uint32_t pid,
                                    int32_t flag,
                                    base::StringView name,
                                    uint32_t tgid,
                                    int64_t value) {
  systrace_utils::SystraceTracePoint point{};
  point.name = name;
  point.int_value = value;
  point.tgid = tgid;

  // The value of these constants can be found in the msm-google kernel.
  constexpr int32_t kSystraceEventBegin = 1 << 0;
  constexpr int32_t kSystraceEventEnd = 1 << 1;
  constexpr int32_t kSystraceEventInt64 = 1 << 2;

  if ((flag & kSystraceEventBegin) != 0) {
    point.phase = 'B';
  } else if ((flag & kSystraceEventEnd) != 0) {
    point.phase = 'E';
  } else if ((flag & kSystraceEventInt64) != 0) {
    point.phase = 'C';
  } else {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }
  // Note: for counter (C) events, we cannot assume that pid is within tgid.
  // See ParseKernelTracingMarkWrite for rationale.
  ParseSystracePoint(ts, pid, point);
}

void SystraceParser::ParseKernelTracingMarkWrite(int64_t ts,
                                                 uint32_t pid,
                                                 char trace_type,
                                                 bool trace_begin,
                                                 base::StringView trace_name,
                                                 uint32_t tgid,
                                                 int64_t value) {
  systrace_utils::SystraceTracePoint point{};
  point.name = trace_name;
  point.int_value = value;
  point.tgid = tgid;

  // Some versions of this trace point fill trace_type with one of (B/E/C),
  // others use the trace_begin boolean and only support begin/end events:
  if (trace_type == 0) {
    point.phase = trace_begin ? 'B' : 'E';
  } else if (trace_type == 'B' || trace_type == 'E' || trace_type == 'C') {
    point.phase = trace_type;
  } else {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }

  // Note: |pid| is the thread id of the emitting thread, |tgid| is taken from
  // the event payload. The begin/end event kernel atrace macros seem well
  // behaved (i.e. they always put current->tgid into the payload). However the
  // counter events have cases where a placeholder/unrelated pid is used (e.g.
  // 0, 1, or a specific kthread, see g2d_frame_* counters for an example).
  //
  // Further, the counter events expect to be grouped at the process (tgid)
  // level (multiple distinct pids will be emitting values for the same
  // logical counter).
  //
  // Therefore we must never assume that pid is within tgid for counter events,
  // but still trust that the tgid value is for a valid process (which will
  // usually fall onto swapper/init or some kthread) to have a
  // process_counter_track for the counter values.
  ParseSystracePoint(ts, pid, point);
}

// TODO(rsavitski): try to remove most special casing of tgid 0, as it is valid
// for kernel systrace points (due to systrace from interrupts).
void SystraceParser::ParseSystracePoint(
    int64_t ts,
    uint32_t pid,
    systrace_utils::SystraceTracePoint point) {
  auto get_utid = [pid, &point, this]() {
    if (point.tgid == 0)
      return context_->process_tracker->GetOrCreateThread(pid);
    return context_->process_tracker->UpdateThread(pid, point.tgid);
  };

  switch (point.phase) {
    case 'B': {
      StringId name_id = context_->storage->InternString(point.name);
      UniqueTid utid = get_utid();
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      context_->slice_tracker->Begin(ts, track_id, kNullStringId /* cat */,
                                     name_id);
      PostProcessSpecialSliceBegin(ts, point.name);
      break;
    }

    case 'E': {
      // |point.tgid| can be 0 in older android versions where the end event
      // would not contain the value.
      UniqueTid utid;
      if (point.tgid == 0) {
        // If we haven't seen this thread before, there can't have been a Begin
        // event for it so just ignore the event.
        auto opt_utid = context_->process_tracker->GetThreadOrNull(pid);
        if (!opt_utid)
          break;
        utid = *opt_utid;
      } else {
        utid = context_->process_tracker->UpdateThread(pid, point.tgid);
      }
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      context_->slice_tracker->End(ts, track_id);
      break;
    }

    case 'S':
    case 'F': {
      StringId name_id = context_->storage->InternString(point.name);
      int64_t cookie = point.int_value;
      UniquePid upid =
          context_->process_tracker->GetOrCreateProcess(point.tgid);

      auto track_set_id =
          context_->async_track_set_tracker
              ->InternAndroidLegacyUnnestableTrackSet(upid, name_id);

      if (point.phase == 'S') {
        // Historically, async slices on Android did not support nesting async
        // slices (i.e. you could not have a stack of async slices). If clients
        // were implemented correctly, we would simply be able to use the normal
        // Begin method and we could rely on the traced code to never emit two
        // 'S' events back to back on the same track.
        // However, there exists buggy code in Android (in Wakelock class of
        // PowerManager) which emits an arbitrary number of 'S' events and
        // expects only the first one to be tracked. Moreover, this issue is
        // compounded by an unfortunate implementation of async slices in
        // Catapult (the legacy trace viewer) which simply tracks the details of
        // the *most recent* emitted 'S' event which leads even more inaccurate
        // behaviour. To support these quirks, we have the special 'unnestable'
        // slice concept which implements workarounds for these very specific
        // issues. No other code should ever use |BeginLegacyUnnestable|.
        tables::SliceTable::Row row;
        row.ts = ts;
        row.track_id =
            context_->async_track_set_tracker->Begin(track_set_id, cookie);
        row.name = name_id;
        context_->slice_tracker->BeginLegacyUnnestable(
            row, [this, cookie](ArgsTracker::BoundInserter* inserter) {
              inserter->AddArg(cookie_id_, Variadic::Integer(cookie));
            });
      } else {
        TrackId track_id =
            context_->async_track_set_tracker->End(track_set_id, cookie);
        context_->slice_tracker->End(ts, track_id);
      }
      break;
    }

    case 'I': {
      StringId name_id = context_->storage->InternString(point.name);
      UniqueTid utid = get_utid();
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      context_->slice_tracker->Scoped(ts, track_id, kNullStringId, name_id, 0);
      break;
    }

    case 'N':
    case 'G':
    case 'H': {
      StringId name_id = context_->storage->InternString(point.name);
      StringId track_name_id = context_->storage->InternString(point.str_value);
      UniquePid upid =
          context_->process_tracker->GetOrCreateProcess(point.tgid);

      // Promote DeviceStateChanged to its own top level track.
      AsyncTrackSetTracker::TrackSetId track_set_id;
      if (point.str_value == "DeviceStateChanged") {
        track_set_id = context_->async_track_set_tracker->InternGlobalTrackSet(
            track_name_id);
      } else {
        track_set_id = context_->async_track_set_tracker->InternProcessTrackSet(
            upid, track_name_id);
      }

      if (point.phase == 'N') {
        TrackId track_id =
            context_->async_track_set_tracker->Scoped(track_set_id, ts, 0);
        context_->slice_tracker->Scoped(ts, track_id, kNullStringId, name_id,
                                        0);
      } else if (point.phase == 'G') {
        TrackId track_id = context_->async_track_set_tracker->Begin(
            track_set_id, point.int_value);
        context_->slice_tracker->Begin(ts, track_id, kNullStringId, name_id);
      } else if (point.phase == 'H') {
        TrackId track_id = context_->async_track_set_tracker->End(
            track_set_id, point.int_value);
        context_->slice_tracker->End(ts, track_id);
      }
      break;
    }

    // Warning: counter event handling must never assume that the |pid| thread
    // is within the |tgid| process due to kernel systrace quirks. If you need
    // to change this, update ParseKernelTracingMarkWrite and ParseZeroEvent to
    // pretend that pid is the same as tgid for C events.
    case 'C': {
      // LMK events from userspace are hacked as counter events with the "value"
      // of the counter representing the pid of the killed process which is
      // reset to 0 once the kill is complete.
      // Homogenise this with kernel LMK events as an instant event, ignoring
      // the resets to 0.
      if (point.name == "kill_one_process") {
        auto killed_pid = static_cast<uint32_t>(point.int_value);
        if (killed_pid != 0) {
          UniquePid killed_upid =
              context_->process_tracker->GetOrCreateProcess(killed_pid);
          TrackId track =
              context_->track_tracker->InternProcessTrack(killed_upid);
          context_->slice_tracker->Scoped(ts, track, kNullStringId, lmk_id_, 0);
        }
        // TODO(lalitm): we should not add LMK events to the counters table
        // once the UI has support for displaying instants.
      } else if (point.name == "ScreenState") {
        // Promote ScreenState to its own top level counter.
        TrackId track = context_->track_tracker->InternGlobalCounterTrack(
            TrackTracker::Group::kDeviceState, screen_state_id_);
        context_->event_tracker->PushCounter(
            ts, static_cast<double>(point.int_value), track);
        return;
      } else if (point.name.StartsWith("battery_stats.")) {
        // Promote battery_stats conters to global tracks.
        StringId name_id = context_->storage->InternString(point.name);
        TrackId track = context_->track_tracker->InternGlobalCounterTrack(
            TrackTracker::Group::kPower, name_id);
        context_->event_tracker->PushCounter(
            ts, static_cast<double>(point.int_value), track);
        return;
      }

      // This is per upid on purpose. Some long-standing counters are pushed
      // from arbitrary threads but expect to be per process (b/123560328).
      // This affects both userspace and kernel counters.
      StringId name_id = context_->storage->InternString(point.name);
      UniquePid upid =
          context_->process_tracker->GetOrCreateProcess(point.tgid);
      TrackId track_id =
          context_->track_tracker->InternProcessCounterTrack(name_id, upid);
      context_->event_tracker->PushCounter(
          ts, static_cast<double>(point.int_value), track_id);
    }
  }
}

void SystraceParser::PostProcessSpecialSliceBegin(int64_t ts,
                                                  base::StringView name) {
  if (name.StartsWith("lmk,")) {
    // LMK events introduced with http://aosp/1782391 are treated specially
    // to parse the killed process oom_score_adj out of them.
    // Format is 'lmk,pid,reason,oom adj,...'
    std::vector<std::string> toks = base::SplitString(name.ToStdString(), ",");
    if (toks.size() < 4) {
      return;
    }
    auto killed_pid = base::StringToUInt32(toks[1]);
    auto oom_score_adj = base::StringToInt32(toks[3]);
    if (!killed_pid || !oom_score_adj) {
      return;
    }

    UniquePid killed_upid =
        context_->process_tracker->GetOrCreateProcess(*killed_pid);
    // Add the oom score entry
    TrackId counter_track = context_->track_tracker->InternProcessCounterTrack(
        oom_score_adj_id_, killed_upid);
    context_->event_tracker->PushCounter(ts, *oom_score_adj, counter_track);

    // Add mem.lmk instant event for consistency with other methods.
    TrackId track = context_->track_tracker->InternProcessTrack(killed_upid);
    context_->slice_tracker->Scoped(ts, track, kNullStringId, lmk_id_, 0);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
