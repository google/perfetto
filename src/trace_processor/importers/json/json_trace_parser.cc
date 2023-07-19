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

#include "src/trace_processor/importers/json/json_trace_parser.h"

#include <cinttypes>
#include <limits>
#include <optional>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
namespace {

std::optional<uint64_t> MaybeExtractFlowIdentifier(const Json::Value& value,
                                                   bool version2) {
  std::string id_key = (version2 ? "bind_id" : "id");
  if (!value.isMember(id_key))
    return std::nullopt;
  auto id = value[id_key];
  if (id.isNumeric())
    return id.asUInt64();
  if (!id.isString())
    return std::nullopt;
  const char* c_string = id.asCString();
  return base::CStringToUInt64(c_string, 16);
}

}  // namespace
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)

JsonTraceParser::JsonTraceParser(TraceProcessorContext* context)
    : context_(context), systrace_line_parser_(context) {}

JsonTraceParser::~JsonTraceParser() = default;

void JsonTraceParser::ParseSystraceLine(int64_t, SystraceLine line) {
  systrace_line_parser_.ParseLine(line);
}

void JsonTraceParser::ParseJsonPacket(int64_t timestamp,
                                      std::string string_value) {
  PERFETTO_DCHECK(json::IsJsonSupported());

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
  auto opt_value = json::ParseJsonString(base::StringView(string_value));
  if (!opt_value) {
    context_->storage->IncrementStats(stats::json_parser_failure);
    return;
  }

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  SliceTracker* slice_tracker = context_->slice_tracker.get();
  FlowTracker* flow_tracker = context_->flow_tracker.get();

  const Json::Value& value = *opt_value;
  auto& ph = value["ph"];
  if (!ph.isString())
    return;
  char phase = *ph.asCString();

  std::optional<uint32_t> opt_pid;
  std::optional<uint32_t> opt_tid;

  if (value.isMember("pid"))
    opt_pid = json::CoerceToUint32(value["pid"]);
  if (value.isMember("tid"))
    opt_tid = json::CoerceToUint32(value["tid"]);

  uint32_t pid = opt_pid.value_or(0);
  uint32_t tid = opt_tid.value_or(pid);
  UniqueTid utid = procs->UpdateThread(tid, pid);

  std::string id = value.isMember("id") ? value["id"].asString() : "";

  base::StringView cat = value.isMember("cat")
                             ? base::StringView(value["cat"].asCString())
                             : base::StringView();
  StringId cat_id = storage->InternString(cat);

  base::StringView name = value.isMember("name")
                              ? base::StringView(value["name"].asCString())
                              : base::StringView();
  StringId name_id = name.empty() ? kNullStringId : storage->InternString(name);

  auto args_inserter = [this, &value](ArgsTracker::BoundInserter* inserter) {
    if (value.isMember("args")) {
      json::AddJsonValueToArgs(value["args"], /* flat_key = */ "args",
                               /* key = */ "args", context_->storage.get(),
                               inserter);
    }
  };

  // Only used for 'B', 'E', and 'X' events so wrap in lambda so it gets
  // ignored in other cases. This lambda is only safe to call within the
  // scope of this function due to the capture by reference.
  auto make_slice_row = [&](TrackId track_id) {
    tables::SliceTable::Row row;
    row.ts = timestamp;
    row.track_id = track_id;
    row.category = cat_id;
    row.name = name_id;
    row.thread_ts = json::CoerceToTs(value["tts"]);
    // tdur will only exist on 'X' events.
    row.thread_dur = json::CoerceToTs(value["tdur"]);
    // JSON traces don't report these counters as part of slices.
    row.thread_instruction_count = std::nullopt;
    row.thread_instruction_delta = std::nullopt;
    return row;
  };

  switch (phase) {
    case 'B': {  // TRACE_EVENT_BEGIN.
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      slice_tracker->BeginTyped(storage->mutable_slice_table(),
                                make_slice_row(track_id), args_inserter);
      MaybeAddFlow(track_id, value);
      break;
    }
    case 'E': {  // TRACE_EVENT_END.
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_slice_id = slice_tracker->End(timestamp, track_id, cat_id,
                                             name_id, args_inserter);
      // Now try to update thread_dur if we have a tts field.
      auto opt_tts = json::CoerceToTs(value["tts"]);
      if (opt_slice_id.has_value() && opt_tts) {
        auto* slice = storage->mutable_slice_table();
        auto maybe_row = slice->id().IndexOf(*opt_slice_id);
        PERFETTO_DCHECK(maybe_row.has_value());
        auto start_tts = slice->thread_ts()[*maybe_row];
        if (start_tts) {
          slice->mutable_thread_dur()->Set(*maybe_row, *opt_tts - *start_tts);
        }
      }
      break;
    }
    case 'b':
    case 'e':
    case 'n': {
      if (!opt_pid || id.empty()) {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
      int64_t cookie = static_cast<int64_t>(base::Hasher::Combine(id.c_str()));
      StringId scope = kNullStringId;
      TrackId track_id = context_->track_tracker->InternLegacyChromeAsyncTrack(
          name_id, upid, cookie, true /* source_id_is_process_scoped */, scope);

      if (phase == 'b') {
        slice_tracker->BeginTyped(storage->mutable_slice_table(),
                                  make_slice_row(track_id), args_inserter);
        MaybeAddFlow(track_id, value);
      } else if (phase == 'e') {
        slice_tracker->End(timestamp, track_id, cat_id, name_id, args_inserter);
        // We don't handle tts here as we do in the 'E'
        // case above as it's not well defined for aysnc slices.
      } else {
        context_->slice_tracker->Scoped(timestamp, track_id, cat_id, name_id, 0,
                                        args_inserter);
        MaybeAddFlow(track_id, value);
      }
      break;
    }
    case 'X': {  // TRACE_EVENT (scoped event).
      std::optional<int64_t> opt_dur = json::CoerceToTs(value["dur"]);
      if (!opt_dur.has_value())
        return;
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto row = make_slice_row(track_id);
      row.dur = opt_dur.value();
      slice_tracker->ScopedTyped(storage->mutable_slice_table(), std::move(row),
                                 args_inserter);
      MaybeAddFlow(track_id, value);
      break;
    }
    case 'C': {  // TRACE_EVENT_COUNTER
      auto args = value["args"];
      if (!args.isObject()) {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }

      std::string counter_name_prefix = name.ToStdString();
      if (!id.empty()) {
        counter_name_prefix += " id: " + id;
      }

      for (auto it = args.begin(); it != args.end(); ++it) {
        double counter;
        if (it->isString()) {
          auto opt = base::CStringToDouble(it->asCString());
          if (!opt.has_value()) {
            context_->storage->IncrementStats(stats::json_parser_failure);
            continue;
          }
          counter = opt.value();
        } else if (it->isNumeric()) {
          counter = it->asDouble();
        } else {
          context_->storage->IncrementStats(stats::json_parser_failure);
          continue;
        }
        std::string counter_name = counter_name_prefix + " " + it.name();
        StringId counter_name_id =
            context_->storage->InternString(base::StringView(counter_name));
        context_->event_tracker->PushProcessCounterForThread(
            timestamp, counter, counter_name_id, utid);
      }
      break;
    }
    case 'R':
    case 'I':
    case 'i': {  // TRACE_EVENT_INSTANT
      base::StringView scope;
      if (value.isMember("s")) {
        scope = value["s"].asCString();
      }

      TrackId track_id;
      if (scope == "g") {
        track_id = context_->track_tracker
                       ->GetOrCreateLegacyChromeGlobalInstantTrack();
      } else if (scope == "p") {
        if (!opt_pid) {
          context_->storage->IncrementStats(stats::json_parser_failure);
          break;
        }
        UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
        track_id =
            context_->track_tracker->InternLegacyChromeProcessInstantTrack(
                upid);
      } else if (scope == "t" || scope.data() == nullptr) {
        if (!opt_tid) {
          context_->storage->IncrementStats(stats::json_parser_failure);
          break;
        }
        track_id = context_->track_tracker->InternThreadTrack(utid);
        auto row = make_slice_row(track_id);
        row.dur = 0;
        if (row.thread_ts) {
          // Only set thread_dur to zero if we have a thread_ts.
          row.thread_dur = 0;
        }
        slice_tracker->ScopedTyped(storage->mutable_slice_table(),
                                   std::move(row), args_inserter);
        break;
      } else {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }
      context_->slice_tracker->Scoped(timestamp, track_id, cat_id, name_id, 0,
                                      args_inserter);
      break;
    }
    case 's': {  // TRACE_EVENT_FLOW_START
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_source_id =
          MaybeExtractFlowIdentifier(value, /* version2 = */ false);
      if (opt_source_id) {
        FlowId flow_id = flow_tracker->GetFlowIdForV1Event(
            opt_source_id.value(), cat_id, name_id);
        flow_tracker->Begin(track_id, flow_id);
      } else {
        context_->storage->IncrementStats(stats::flow_invalid_id);
      }
      break;
    }
    case 't': {  // TRACE_EVENT_FLOW_STEP
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_source_id =
          MaybeExtractFlowIdentifier(value, /* version2 = */ false);
      if (opt_source_id) {
        FlowId flow_id = flow_tracker->GetFlowIdForV1Event(
            opt_source_id.value(), cat_id, name_id);
        flow_tracker->Step(track_id, flow_id);
      } else {
        context_->storage->IncrementStats(stats::flow_invalid_id);
      }
      break;
    }
    case 'f': {  // TRACE_EVENT_FLOW_END
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_source_id =
          MaybeExtractFlowIdentifier(value, /* version2 = */ false);
      if (opt_source_id) {
        FlowId flow_id = flow_tracker->GetFlowIdForV1Event(
            opt_source_id.value(), cat_id, name_id);
        bool bind_enclosing_slice =
            value.isMember("bp") && strcmp(value["bp"].asCString(), "e") == 0;
        flow_tracker->End(track_id, flow_id, bind_enclosing_slice,
                          /* close_flow = */ false);
      } else {
        context_->storage->IncrementStats(stats::flow_invalid_id);
      }
      break;
    }
    case 'M': {  // Metadata events (process and thread names).
      if (name == "thread_name" && !value["args"]["name"].empty()) {
        const char* thread_name = value["args"]["name"].asCString();
        auto thread_name_id = context_->storage->InternString(thread_name);
        procs->UpdateThreadName(tid, thread_name_id,
                                ThreadNamePriority::kOther);
        break;
      }
      if (name == "process_name" && !value["args"]["name"].empty()) {
        const char* proc_name = value["args"]["name"].asCString();
        procs->SetProcessMetadata(pid, std::nullopt, proc_name,
                                  base::StringView());
        break;
      }
    }
  }
#else
  perfetto::base::ignore_result(timestamp);
  perfetto::base::ignore_result(context_);
  perfetto::base::ignore_result(string_value);
  PERFETTO_ELOG("Cannot parse JSON trace due to missing JSON support");
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
}

void JsonTraceParser::MaybeAddFlow(TrackId track_id, const Json::Value& event) {
  PERFETTO_DCHECK(json::IsJsonSupported());
#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
  auto opt_bind_id = MaybeExtractFlowIdentifier(event, /* version2 = */ true);
  if (opt_bind_id) {
    FlowTracker* flow_tracker = context_->flow_tracker.get();
    bool flow_out = event.isMember("flow_out") && event["flow_out"].asBool();
    bool flow_in = event.isMember("flow_in") && event["flow_in"].asBool();
    if (flow_in && flow_out) {
      flow_tracker->Step(track_id, opt_bind_id.value());
    } else if (flow_out) {
      flow_tracker->Begin(track_id, opt_bind_id.value());
    } else if (flow_in) {
      // bind_enclosing_slice is always true for v2 flow events
      flow_tracker->End(track_id, opt_bind_id.value(), true,
                        /* close_flow = */ false);
    } else {
      context_->storage->IncrementStats(stats::flow_without_direction);
    }
  }
#else
  perfetto::base::ignore_result(track_id);
  perfetto::base::ignore_result(event);
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
}

}  // namespace trace_processor
}  // namespace perfetto
