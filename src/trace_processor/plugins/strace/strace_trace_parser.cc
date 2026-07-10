/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/strace/strace_trace_parser.h"

#include <cstdint>
#include <optional>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/plugins/strace/strace_event.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor::strace_importer {

namespace {

void AddArgs(ArgsTracker::BoundInserter* inserter,
             StringId args_key,
             std::optional<StringId> args_id,
             StringId ret_key,
             std::optional<StringId> return_value_id) {
  if (args_id) {
    inserter->AddArg(args_key, Variadic::String(*args_id));
  }
  if (return_value_id) {
    inserter->AddArg(ret_key, Variadic::String(*return_value_id));
  }
}

}  // namespace

StraceTraceParser::StraceTraceParser(TraceProcessorContext* context)
    : context_(context),
      category_id_(context->storage->InternString("strace")),
      args_key_id_(context->storage->InternString("args")),
      ret_key_id_(context->storage->InternString("ret")) {}

StraceTraceParser::~StraceTraceParser() = default;

void StraceTraceParser::Parse(int64_t ts, StraceEvent evt) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(evt.tid);
  TrackId track = context_->track_tracker->InternThreadTrack(utid);

  auto args_cb = [&](ArgsTracker::BoundInserter* inserter) {
    AddArgs(inserter, args_key_id_, evt.args_id, ret_key_id_,
            evt.return_value_id);
  };

  if (evt.is_unfinished) {
    std::optional<SliceId> id = context_->slice_tracker->Begin(
        ts, track, category_id_, evt.syscall_name_id, args_cb);
    if (id) {
      unfinished_by_tid_.Insert(evt.tid, *id);
    }
    return;
  }

  if (evt.is_resumed) {
    // We don't strictly need the stored SliceId (End() closes the topmost
    // open slice on the track), but tracking it lets us detect and count
    // "resumed" lines with no matching "unfinished" line.
    if (unfinished_by_tid_.Find(evt.tid) == nullptr) {
      context_->stats_tracker->IncrementStats(stats::strace_unmatched_resume);
    } else {
      unfinished_by_tid_.Erase(evt.tid);
    }
    context_->slice_tracker->End(ts, track, category_id_, evt.syscall_name_id,
                                 args_cb);
    return;
  }

  // A complete, single-line call.
  context_->slice_tracker->Scoped(ts, track, category_id_, evt.syscall_name_id,
                                  /*duration=*/0, args_cb);
}

}  // namespace perfetto::trace_processor::strace_importer
