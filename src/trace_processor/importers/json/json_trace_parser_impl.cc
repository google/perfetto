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

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/legacy_v8_cpu_profile_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/json/json_parser.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

namespace {

constexpr uint32_t KeyToInt(std::string_view s) {
  size_t len = s.size();
  uint32_t val = 0;
  if (len > 0)
    val |= static_cast<uint32_t>(s[0]);
  if (len > 1)
    val |= (static_cast<uint32_t>(s[1]) << 8);
  if (len > 2)
    val |= (static_cast<uint32_t>(s[2]) << 16);
  if (len > 3)
    val |= (static_cast<uint32_t>(s[3]) << 24);
  return val;
}

template <size_t N>
constexpr auto BuildIntsArray(const std::array<std::string_view, N>& keys_sv) {
  std::array<uint32_t, N> int_keys_arr{};
  for (size_t i = 0; i < N; ++i) {
    int_keys_arr[i] = KeyToInt(keys_sv[i]);
  }
  return int_keys_arr;
}

constexpr std::array<std::string_view, 13> kL24Keys = {{
    "s",
    "id",
    "ph",
    "bp",
    "id2",
    "pid",
    "tid",
    "cat",
    "tts",
    "dur",
    "args",
    "name",
    "tdur",
}};
constexpr auto kL24Ints = BuildIntsArray(kL24Keys);
constexpr auto kL24Offset = 0;

constexpr std::array<std::string_view, 5> kLNKeys = {{
    "bind_id",
    "local",
    "global",
    "flow_in",
    "flow_out",
}};
constexpr auto kLNOffset = kL24Offset + kL24Keys.size();
constexpr auto kKeysSize = kL24Keys.size() + kLNKeys.size();

constexpr uint32_t IndexOf(std::string_view key) {
  uint32_t cmp = KeyToInt(key);
  if (PERFETTO_LIKELY(key.size() <= 4)) {
    for (uint32_t i = 0; i < kL24Ints.size(); ++i) {
      if (kL24Ints[i] == cmp) {
        return kL24Offset + i;
      }
    }
  }
  for (uint32_t i = 0; i < kLNKeys.size(); ++i) {
    if (kLNKeys[i] == key) {
      return kLNOffset + i;
    }
  }
  return std::numeric_limits<uint32_t>::max();
}

void Assign(std::string_view key,
            const json::JsonValue& value,
            std::array<json::JsonValue, kKeysSize>& res) {
  uint32_t cmp = KeyToInt(key);
  if (PERFETTO_LIKELY(key.size() <= 4)) {
    for (uint32_t i = 0; i < kL24Ints.size(); ++i) {
      if (kL24Ints[i] == cmp) {
        res[kL24Offset + i] = value;
        return;
      }
    }
  }
  for (uint32_t i = 0; i < kLNKeys.size(); ++i) {
    if (kLNKeys[i] == key) {
      res[kLNOffset + i] = value;
      return;
    }
  }
}

constexpr uint32_t FlowIdIndexOf(bool version2) {
  return version2 ? IndexOf("bind_id") : IndexOf("id");
}

std::optional<uint64_t> MaybeExtractFlowIdentifier(
    const std::array<json::JsonValue, kKeysSize>& values,
    uint32_t index) {
  switch (values[index].index()) {
    case base::variant_index<json::JsonValue, double>():
      return static_cast<uint64_t>(base::unchecked_get<double>(values[index]));
    case base::variant_index<json::JsonValue, int64_t>():
      return static_cast<uint64_t>(base::unchecked_get<int64_t>(values[index]));
    case base::variant_index<json::JsonValue, json::SimpleJsonString>(): {
      std::string_view id =
          base::unchecked_get<json::SimpleJsonString>(values[index]).data;
      return base::StringViewToUInt64(base::StringView(id.data(), id.size()),
                                      16);
    }
    case base::variant_index<json::JsonValue, json::ComplexJsonString>(): {
      const std::string& id =
          base::unchecked_get<json::ComplexJsonString>(values[index]).data;
      return base::CStringToUInt64(id.c_str(), 16);
    }
  }
  return std::nullopt;
}

}  // namespace

JsonTraceParserImpl::JsonTraceParserImpl(TraceProcessorContext* context)
    : context_(context), systrace_line_parser_(context) {}

JsonTraceParserImpl::~JsonTraceParserImpl() = default;

void JsonTraceParserImpl::ParseSystraceLine(int64_t, SystraceLine line) {
  systrace_line_parser_.ParseLine(line);
}

struct ParsedEvent {};

void JsonTraceParserImpl::ParseJsonPacket(int64_t timestamp,
                                          std::string string_value) {
  PERFETTO_DCHECK(json::IsJsonSupported());

  std::array<json::JsonValue, kKeysSize> values;
  json::JsonObjectFieldIterator it(string_value);
  for (; it; ++it) {
    if (it.error_code() != json::JsonParseError::kSuccess) {
      context_->storage->IncrementStats(stats::json_parser_failure);
      return;
    }
    Assign(it.key(), it.value(), values);
  }
  if (it.error_code() != json::JsonParseError::kSuccess) {
    context_->storage->IncrementStats(stats::json_parser_failure);
    return;
  }

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  SliceTracker* slice_tracker = context_->slice_tracker.get();
  FlowTracker* flow_tracker = context_->flow_tracker.get();

  std::string_view ph_str = json::GetStringValue(values[IndexOf("ph")]);
  if (PERFETTO_UNLIKELY(ph_str.size() != 1)) {
    context_->storage->IncrementStats(stats::json_parser_failure);
    return;
  }
  char phase = ph_str[0];

  std::optional<uint32_t> opt_pid;
  if (const auto& pid_value = values[IndexOf("pid")]; json::Exists(pid_value)) {
    std::string_view proc_name = json::GetStringValue(pid_value);
    if (!proc_name.empty()) {
      // If the pid is a string, treat raw id of the interned string as the pid.
      // This "hack" which allows emitting "quick-and-dirty" compact JSON
      // traces: relying on these traces for production is necessarily brittle
      // as it is not a part of the actual spec.
      opt_pid = storage->InternString(proc_name).raw_id();
      procs->SetProcessMetadata(
          *opt_pid, std::nullopt,
          base::StringView(proc_name.data(), proc_name.size()),
          base::StringView());
    } else {
      opt_pid = json::CoerceToUint32(pid_value);
    }
  }

  std::optional<uint32_t> opt_tid;
  if (const auto& tid_value = values[IndexOf("tid")]; json::Exists(tid_value)) {
    std::string_view thread_name = json::GetStringValue(tid_value);
    if (!thread_name.empty()) {
      // See the comment for |pid| string handling above: the same applies here.
      StringId thread_name_id = storage->InternString(thread_name);
      opt_tid = thread_name_id.raw_id();
      procs->UpdateThreadName(*opt_tid, thread_name_id,
                              ThreadNamePriority::kOther);
    } else {
      opt_tid = json::CoerceToUint32(tid_value);
    }
  }

  uint32_t pid = opt_pid.value_or(0);
  uint32_t tid = opt_tid.value_or(pid);
  UniqueTid utid = procs->UpdateThread(tid, pid);

  std::string_view id = json::GetStringValue(values[IndexOf("id")]);
  StringId cat =
      storage->InternString(json::GetStringValue(values[IndexOf("cat")]));

  std::string_view name = json::GetStringValue(values[IndexOf("name")]);
  StringId name_id = name.empty() ? kNullStringId : storage->InternString(name);

  auto args_inserter = [](ArgsTracker::BoundInserter*) {
    // if (value.isMember("args")) {
    //   json::AddJsonValueToArgs(value["args"], /* flat_key = */ "args",
    //                            /* key = */ "args", context_->storage.get(),
    //                            inserter);
    // }
  };

  // Only used for 'B', 'E', and 'X' events so wrap in lambda so it gets
  // ignored in other cases. This lambda is only safe to call within the
  // scope of this function due to the capture by reference.
  StringId slice_name_id =
      name_id == kNullStringId ? storage->InternString("[No name]") : name_id;
  switch (phase) {
    case 'B': {  // TRACE_EVENT_BEGIN.
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto slice_id = slice_tracker->Begin(timestamp, track_id, cat,
                                           slice_name_id, args_inserter);
      if (slice_id) {
        if (auto tts = json::CoerceToTs(values[IndexOf("tts")]); tts) {
          auto rr =
              context_->storage->mutable_slice_table()->FindById(*slice_id);
          rr->set_thread_ts(*tts);
        }
      }
      MaybeAddFlow(track_id, values);
      break;
    }
    case 'E': {  // TRACE_EVENT_END.
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_slice_id =
          slice_tracker->End(timestamp, track_id, cat, name_id, args_inserter);
      // Now try to update thread_dur if we have a tts field.
      auto opt_tts = json::CoerceToTs(values[IndexOf("tts")]);
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
      // const json::JsonValue& id2 = values[IndexOf("id2")];
      // TODO: DNS this should be from id2
      std::string_view local = json::GetStringValue(values[IndexOf("local")]);
      std::string_view global = json::GetStringValue(values[IndexOf("global")]);
      if (!opt_pid || (id.empty() && global.empty() && local.empty())) {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
      TrackId track_id;
      if (!id.empty() || !global.empty()) {
        std::string_view real_id = id.empty() ? global : id;
        int64_t cookie =
            static_cast<int64_t>(base::Hasher::Combine(cat.raw_id(), real_id));
        track_id = context_->track_tracker->InternLegacyAsyncTrack(
            name_id, upid, cookie, false /* source_id_is_process_scoped */,
            kNullStringId /* source_scope */);
      } else {
        PERFETTO_DCHECK(!local.empty());
        int64_t cookie =
            static_cast<int64_t>(base::Hasher::Combine(cat.raw_id(), local));
        track_id = context_->track_tracker->InternLegacyAsyncTrack(
            name_id, upid, cookie, true /* source_id_is_process_scoped */,
            kNullStringId /* source_scope */);
      }

      if (phase == 'b') {
        slice_tracker->Begin(timestamp, track_id, cat, slice_name_id,
                             args_inserter);
        MaybeAddFlow(track_id, values);
      } else if (phase == 'e') {
        slice_tracker->End(timestamp, track_id, cat, name_id, args_inserter);
        // We don't handle tts here as we do in the 'E'
        // case above as it's not well defined for async slices.
      } else {
        context_->slice_tracker->Scoped(timestamp, track_id, cat, name_id, 0,
                                        args_inserter);
        MaybeAddFlow(track_id, values);
      }
      break;
    }
    case 'X': {  // TRACE_EVENT (scoped event).
      std::optional<int64_t> opt_dur = json::CoerceToTs(values[IndexOf("dur")]);
      if (!opt_dur.has_value())
        return;
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto slice_id = slice_tracker->Scoped(
          timestamp, track_id, cat, slice_name_id, *opt_dur, args_inserter);
      if (slice_id) {
        auto rr = context_->storage->mutable_slice_table()->FindById(*slice_id);
        if (auto tts = json::CoerceToTs(values[IndexOf("tts")]); tts) {
          rr->set_thread_ts(*tts);
        }
        if (auto tdur = json::CoerceToTs(values[IndexOf("tdur")]); tdur) {
          rr->set_thread_dur(*tdur);
        }
      }
      MaybeAddFlow(track_id, values);
      break;
    }
    case 'C': {  // TRACE_EVENT_COUNTER
      const auto* args = json::GetObject(values[IndexOf("args")]);
      if (!args) {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }

      std::string counter_name_prefix(name);
      if (!id.empty()) {
        counter_name_prefix += " id: ";
        counter_name_prefix += id;
      }
      counter_name_prefix += " ";

      json::JsonObjectFieldIterator args_it(args->raw_data);
      for (; args_it; ++args_it) {
        double counter;
        const json::JsonValue& val = args_it.value();
        switch (val.index()) {
          case base::variant_index<json::JsonValue, double>():
            counter = base::unchecked_get<double>(val);
            break;
          case base::variant_index<json::JsonValue, int64_t>():
            counter = static_cast<double>(base::unchecked_get<int64_t>(val));
            break;
          case base::variant_index<json::JsonValue, json::SimpleJsonString>(): {
            std::string_view str =
                base::unchecked_get<json::SimpleJsonString>(val).data;
            auto opt_counter = base::StringToDouble(std::string(str));
            if (!opt_counter) {
              context_->storage->IncrementStats(stats::json_parser_failure);
              continue;
            }
            counter = *opt_counter;
            break;
          }
          case base::variant_index<json::JsonValue,
                                   json::ComplexJsonString>(): {
            const std::string& str =
                base::unchecked_get<json::ComplexJsonString>(val).data;
            auto opt_counter = base::StringToDouble(str);
            if (!opt_counter) {
              context_->storage->IncrementStats(stats::json_parser_failure);
              continue;
            }
            counter = *opt_counter;
            break;
          }
          default:
            context_->storage->IncrementStats(stats::json_parser_failure);
            continue;
        }
        std::string counter_name = counter_name_prefix;
        counter_name += args_it.key();
        StringId nid = context_->storage->InternString(counter_name);
        context_->event_tracker->PushProcessCounterForThread(
            EventTracker::JsonCounter{nid}, timestamp, counter, utid);
      }
      if (args_it.error_code() != json::JsonParseError::kSuccess) {
        context_->storage->IncrementStats(stats::json_parser_failure);
        return;
      }
      break;
    }
    case 'R':
    case 'I':
    case 'i': {  // TRACE_EVENT_INSTANT
      std::string_view scope = json::GetStringValue(values[IndexOf("s")]);
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
        auto sid = slice_tracker->Scoped(timestamp, track_id, cat,
                                         slice_name_id, 0, args_inserter);
        if (sid) {
          if (auto tts = json::CoerceToTs(values[IndexOf("tts")]); tts) {
            auto rr = context_->storage->mutable_slice_table()->FindById(*sid);
            rr->set_thread_ts(*tts);
          }
        }
        break;
      } else {
        context_->storage->IncrementStats(stats::json_parser_failure);
        break;
      }
      context_->slice_tracker->Scoped(timestamp, track_id, cat, name_id, 0,
                                      args_inserter);
      break;
    }
    case 's': {  // TRACE_EVENT_FLOW_START
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_source_id =
          MaybeExtractFlowIdentifier(values, FlowIdIndexOf(false));
      if (opt_source_id) {
        FlowId flow_id = flow_tracker->GetFlowIdForV1Event(
            opt_source_id.value(), cat, name_id);
        flow_tracker->Begin(track_id, flow_id);
      } else {
        context_->storage->IncrementStats(stats::flow_invalid_id);
      }
      break;
    }
    case 't': {  // TRACE_EVENT_FLOW_STEP
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_source_id =
          MaybeExtractFlowIdentifier(values, FlowIdIndexOf(false));
      if (opt_source_id) {
        FlowId flow_id = flow_tracker->GetFlowIdForV1Event(
            opt_source_id.value(), cat, name_id);
        flow_tracker->Step(track_id, flow_id);
      } else {
        context_->storage->IncrementStats(stats::flow_invalid_id);
      }
      break;
    }
    case 'f': {  // TRACE_EVENT_FLOW_END
      TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
      auto opt_source_id =
          MaybeExtractFlowIdentifier(values, FlowIdIndexOf(false));
      if (opt_source_id) {
        FlowId flow_id = flow_tracker->GetFlowIdForV1Event(
            opt_source_id.value(), cat, name_id);
        bool bind_enclosing_slice =
            json::GetStringValue(values[IndexOf("bp")]) == "b";
        flow_tracker->End(track_id, flow_id, bind_enclosing_slice,
                          /* close_flow = */ false);
      } else {
        context_->storage->IncrementStats(stats::flow_invalid_id);
      }
      break;
    }
    case 'M': {  // Metadata events (process and thread names).
      // if (name == "thread_name" && !value["args"]["name"].empty()) {
      //   const char* thread_name = value["args"]["name"].asCString();
      //   auto thread_name_id = context_->storage->InternString(thread_name);
      //   procs->UpdateThreadName(tid, thread_name_id,
      //                           ThreadNamePriority::kOther);
      //   break;
      // }
      // if (name == "process_name" && !value["args"]["name"].empty()) {
      //   const char* proc_name = value["args"]["name"].asCString();
      //   procs->SetProcessMetadata(pid, std::nullopt, proc_name,
      //                             base::StringView());
      //   break;
      // }
    }
  }
}

void JsonTraceParserImpl::MaybeAddFlow(
    TrackId track_id,
    const std::array<json::JsonValue, kKeysSize>& event) {
  PERFETTO_DCHECK(json::IsJsonSupported());
  auto opt_bind_id = MaybeExtractFlowIdentifier(event, FlowIdIndexOf(true));
  if (opt_bind_id) {
    FlowTracker* flow_tracker = context_->flow_tracker.get();
    const auto& flow_out_val = event[IndexOf("flow_out")];
    const auto& flow_in_val = event[IndexOf("flow_in")];
    bool flow_out = json::CoerceToBool(flow_out_val);
    bool flow_in = json::CoerceToBool(flow_in_val);
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
