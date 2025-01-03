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

#include "src/trace_processor/importers/json/json_trace_parser_impl.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/legacy_v8_cpu_profile_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

namespace {

std::optional<uint64_t> MaybeExtractFlowIdentifier(const Json::Value& value,
                                                   bool version2) {
  std::string id_key = (version2 ? "bind_id" : "id");
  if (!value.isMember(id_key))
    return std::nullopt;
  const auto& id = value[id_key];
  if (id.isNumeric())
    return id.asUInt64();
  if (!id.isString())
    return std::nullopt;
  const char* c_string = id.asCString();
  return base::CStringToUInt64(c_string, 16);
}

}  // namespace

JsonTraceParserImpl::JsonTraceParserImpl(TraceProcessorContext* context)
    : context_(context), systrace_line_parser_(context) {}

JsonTraceParserImpl::~JsonTraceParserImpl() = default;

void JsonTraceParserImpl::ParseSystraceLine(int64_t, SystraceLine line) {
  systrace_line_parser_.ParseLine(line);
}

void JsonTraceParserImpl::ParseJsonPacket(int64_t timestamp,
                                          std::string string_value) {
  PERFETTO_DCHECK(json::IsJsonSupported());

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
  const auto& ph = value["ph"];
  if (!ph.isString())
    return;
  char phase = *ph.asCString();

  std::optional<uint32_t> opt_pid;
  if (value.isMember("pid")) {
    if (value["pid"].isString()) {
      // If the pid is a string, treat raw id of the interned string as the pid.
      // This "hack" which allows emitting "quick-and-dirty" compact JSON
      // traces: relying on these traces for production is necessarily brittle
      // as it is not a part of the actual spec.
      const char* proc_name = value["pid"].asCString();
      opt_pid = storage->InternString(proc_name).raw_id();
      procs->SetProcessMetadata(*opt_pid, std::nullopt, proc_name,
                                base::StringView());
    } else {
      opt_pid = json::CoerceToUint32(value["pid"]);
    }
  }

  std::optional<uint32_t> opt_tid;
  if (value.isMember("tid")) {
    if (value["tid"].isString()) {
      // See the comment for |pid| string handling above: the same applies here.
      StringId thread_name_id = storage->InternString(value["tid"].asCString());
      opt_tid = thread_name_id.raw_id();
      procs->UpdateThreadName(*opt_tid, thread_name_id,
                              ThreadNamePriority::kOther);
    } else {
      opt_tid = json::CoerceToUint32(value["tid"]);
    }
  }

  uint32_t pid = opt_pid.value_or(0);
  uint32_t tid = opt_tid.value_or(pid);
  UniqueTid utid = procs->UpdateThread(tid, pid);

  std::string id = value.isMember("id") ? value["id"].asString() : "";

  base::StringView cat = value.isMember("cat") && value["cat"].isString()
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
  StringId slice_name_id =
      name_id == kNullStringId ? storage->InternString("[No name]") : name_id;
  switch (phase) {
    case 'B': {  // TRACE_EVENT_BEGIN.
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto slice_id = slice_tracker->Begin(timestamp, track_id, cat_id,
                                           slice_name_id, args_inserter);
      if (slice_id) {
        if (auto thread_ts = json::CoerceToTs(value["tts"]); thread_ts) {
          auto rr =
              context_->storage->mutable_slice_table()->FindById(*slice_id);
          rr->set_thread_ts(*thread_ts);
        }
      }
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
        auto rr = *slice->FindById(*opt_slice_id);
        if (auto start_tts = rr.thread_ts(); start_tts) {
          rr.set_thread_dur(*opt_tts - *start_tts);
        }
      }
      break;
    }
    case 'b':
    case 'e':
    case 'n': {
      Json::Value id2 = value.isMember("id2") ? value["id2"] : Json::Value();
      std::string local = id2.isMember("local") ? id2["local"].asString() : "";
      std::string global =
          id2.isMember("global") ? id2["global"].asString() : "";
      if (!opt_pid || (id.empty() && global.empty() && local.empty())) {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
      TrackId track_id;
      if (!id.empty() || !global.empty()) {
        const std::string& real_id = id.empty() ? global : id;
        int64_t cookie = static_cast<int64_t>(
            base::Hasher::Combine(cat_id.raw_id(), real_id));
        track_id = context_->track_tracker->InternLegacyAsyncTrack(
            name_id, upid, cookie, false /* source_id_is_process_scoped */,
            kNullStringId /* source_scope */);
      } else {
        PERFETTO_DCHECK(!local.empty());
        int64_t cookie =
            static_cast<int64_t>(base::Hasher::Combine(cat_id.raw_id(), local));
        track_id = context_->track_tracker->InternLegacyAsyncTrack(
            name_id, upid, cookie, true /* source_id_is_process_scoped */,
            kNullStringId /* source_scope */);
      }

      if (phase == 'b') {
        slice_tracker->Begin(timestamp, track_id, cat_id, slice_name_id,
                             args_inserter);
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
      auto slice_id = slice_tracker->Scoped(
          timestamp, track_id, cat_id, slice_name_id, *opt_dur, args_inserter);
      if (slice_id) {
        auto rr = context_->storage->mutable_slice_table()->FindById(*slice_id);
        if (auto thread_ts = json::CoerceToTs(value["tts"]); thread_ts) {
          rr->set_thread_ts(*thread_ts);
        }
        if (auto thread_dur = json::CoerceToTs(value["tdur"]); thread_dur) {
          rr->set_thread_dur(*thread_dur);
        }
      }
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
        StringId nid = context_->storage->InternString(counter_name);
        context_->event_tracker->PushProcessCounterForThread(
            EventTracker::JsonCounter{nid}, timestamp, counter, utid);
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
        track_id = context_->track_tracker->InternTrack(
            tracks::kLegacyGlobalInstantsBlueprint, tracks::Dimensions(),
            tracks::BlueprintName(),
            [this](ArgsTracker::BoundInserter& inserter) {
              inserter.AddArg(
                  context_->storage->InternString("source"),
                  Variadic::String(context_->storage->InternString("chrome")));
            });
      } else if (scope == "p") {
        if (!opt_pid) {
          context_->storage->IncrementStats(stats::json_parser_failure);
          break;
        }
        UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
        track_id = context_->track_tracker->InternTrack(
            tracks::kChromeProcessInstantBlueprint, tracks::Dimensions(upid),
            tracks::BlueprintName(),
            [this](ArgsTracker::BoundInserter& inserter) {
              inserter.AddArg(
                  context_->storage->InternString("source"),
                  Variadic::String(context_->storage->InternString("chrome")));
            });
      } else if (scope == "t" || scope.data() == nullptr) {
        if (!opt_tid) {
          context_->storage->IncrementStats(stats::json_parser_failure);
          break;
        }
        track_id = context_->track_tracker->InternThreadTrack(utid);
        auto slice_id = slice_tracker->Scoped(timestamp, track_id, cat_id,
                                              slice_name_id, 0, args_inserter);
        if (slice_id) {
          if (auto thread_ts = json::CoerceToTs(value["tts"]); thread_ts) {
            auto rr =
                context_->storage->mutable_slice_table()->FindById(*slice_id);
            rr->set_thread_ts(*thread_ts);
          }
        }
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
}

void JsonTraceParserImpl::MaybeAddFlow(TrackId track_id,
                                       const Json::Value& event) {
  PERFETTO_DCHECK(json::IsJsonSupported());
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
}

void JsonTraceParserImpl::ParseLegacyV8ProfileEvent(
    int64_t ts,
    LegacyV8CpuProfileEvent event) {
  base::Status status = context_->legacy_v8_cpu_profile_tracker->AddSample(
      ts, event.session_id, event.pid, event.tid, event.callsite_id);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::legacy_v8_cpu_profile_invalid_sample);
  }
  context_->args_tracker->Flush();
}

}  // namespace perfetto::trace_processor
