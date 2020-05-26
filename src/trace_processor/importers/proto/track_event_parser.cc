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

#include "src/trace_processor/importers/proto/track_event_parser.h"

#include <string>

#include "perfetto/base/logging.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/importers/proto/args_table_utils.h"
#include "src/trace_processor/importers/proto/chrome_compositor_scheduler_state.descriptor.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/track_tracker.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_compositor_scheduler_state.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_keyed_service.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_legacy_ipc.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_user_event.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/log_message.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "protos/perfetto/trace/track_event/task_execution.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
using protozero::ConstBytes;

// Slices which have been opened but haven't been closed yet will be marked
// with these placeholder values.
constexpr int64_t kPendingThreadDuration = -1;
constexpr int64_t kPendingThreadInstructionDelta = -1;

void AddStringToArgsTable(const char* field,
                          const protozero::ConstChars& str,
                          const ProtoToArgsTable::ParsingOverrideState& state) {
  auto val = state.context->storage->InternString(base::StringView(str));
  auto key = state.context->storage->InternString(base::StringView(field));
  state.args_tracker->AddArg(state.row_id, key, key, Variadic::String(val));
}

bool MaybeParseSourceLocation(
    std::string prefix,
    const ProtoToArgsTable::ParsingOverrideState& state,
    const protozero::Field& field) {
  auto* decoder = state.sequence_state->LookupInternedMessage<
      protos::pbzero::InternedData::kSourceLocationsFieldNumber,
      protos::pbzero::SourceLocation>(state.sequence_generation,
                                      field.as_uint64());
  if (!decoder) {
    // Lookup failed fall back on default behaviour which will just put
    // the source_location_iid into the args table.
    return false;
  }
  {
    ProtoToArgsTable::ScopedStringAppender scoped("file_name", &prefix);
    AddStringToArgsTable(prefix.c_str(), decoder->file_name(), state);
  }
  {
    ProtoToArgsTable::ScopedStringAppender scoped("function_name", &prefix);
    AddStringToArgsTable(prefix.c_str(), decoder->function_name(), state);
  }
  ProtoToArgsTable::ScopedStringAppender scoped("line_number", &prefix);
  auto key = state.context->storage->InternString(base::StringView(prefix));
  state.args_tracker->AddArg(state.row_id, key, key,
                             Variadic::Integer(decoder->line_number()));
  // By returning false we expect this field to be handled like regular.
  return true;
}
}  // namespace

TrackEventParser::TrackEventParser(TraceProcessorContext* context)
    : context_(context),
      task_file_name_args_key_id_(
          context->storage->InternString("task.posted_from.file_name")),
      task_function_name_args_key_id_(
          context->storage->InternString("task.posted_from.function_name")),
      task_line_number_args_key_id_(
          context->storage->InternString("task.posted_from.line_number")),
      log_message_body_key_id_(
          context->storage->InternString("track_event.log_message")),
      raw_legacy_event_id_(
          context->storage->InternString("track_event.legacy_event")),
      legacy_event_original_tid_id_(
          context->storage->InternString("legacy_event.original_tid")),
      legacy_event_category_key_id_(
          context->storage->InternString("legacy_event.category")),
      legacy_event_name_key_id_(
          context->storage->InternString("legacy_event.name")),
      legacy_event_phase_key_id_(
          context->storage->InternString("legacy_event.phase")),
      legacy_event_duration_ns_key_id_(
          context->storage->InternString("legacy_event.duration_ns")),
      legacy_event_thread_timestamp_ns_key_id_(
          context->storage->InternString("legacy_event.thread_timestamp_ns")),
      legacy_event_thread_duration_ns_key_id_(
          context->storage->InternString("legacy_event.thread_duration_ns")),
      legacy_event_thread_instruction_count_key_id_(
          context->storage->InternString(
              "legacy_event.thread_instruction_count")),
      legacy_event_thread_instruction_delta_key_id_(
          context->storage->InternString(
              "legacy_event.thread_instruction_delta")),
      legacy_event_use_async_tts_key_id_(
          context->storage->InternString("legacy_event.use_async_tts")),
      legacy_event_unscoped_id_key_id_(
          context->storage->InternString("legacy_event.unscoped_id")),
      legacy_event_global_id_key_id_(
          context->storage->InternString("legacy_event.global_id")),
      legacy_event_local_id_key_id_(
          context->storage->InternString("legacy_event.local_id")),
      legacy_event_id_scope_key_id_(
          context->storage->InternString("legacy_event.id_scope")),
      legacy_event_bind_id_key_id_(
          context->storage->InternString("legacy_event.bind_id")),
      legacy_event_bind_to_enclosing_key_id_(
          context->storage->InternString("legacy_event.bind_to_enclosing")),
      legacy_event_flow_direction_key_id_(
          context->storage->InternString("legacy_event.flow_direction")),
      flow_direction_value_in_id_(context->storage->InternString("in")),
      flow_direction_value_out_id_(context->storage->InternString("out")),
      flow_direction_value_inout_id_(context->storage->InternString("inout")),
      chrome_user_event_action_args_key_id_(
          context->storage->InternString("user_event.action")),
      chrome_legacy_ipc_class_args_key_id_(
          context->storage->InternString("legacy_ipc.class")),
      chrome_legacy_ipc_line_args_key_id_(
          context->storage->InternString("legacy_ipc.line")),
      chrome_keyed_service_name_args_key_id_(
          context->storage->InternString("keyed_service.name")),
      chrome_legacy_ipc_class_ids_{
          {context->storage->InternString("UNSPECIFIED"),
           context->storage->InternString("AUTOMATION"),
           context->storage->InternString("FRAME"),
           context->storage->InternString("PAGE"),
           context->storage->InternString("VIEW"),
           context->storage->InternString("WIDGET"),
           context->storage->InternString("INPUT"),
           context->storage->InternString("TEST"),
           context->storage->InternString("WORKER"),
           context->storage->InternString("NACL"),
           context->storage->InternString("GPU_CHANNEL"),
           context->storage->InternString("MEDIA"),
           context->storage->InternString("PPAPI"),
           context->storage->InternString("CHROME"),
           context->storage->InternString("DRAG"),
           context->storage->InternString("PRINT"),
           context->storage->InternString("EXTENSION"),
           context->storage->InternString("TEXT_INPUT_CLIENT"),
           context->storage->InternString("BLINK_TEST"),
           context->storage->InternString("ACCESSIBILITY"),
           context->storage->InternString("PRERENDER"),
           context->storage->InternString("CHROMOTING"),
           context->storage->InternString("BROWSER_PLUGIN"),
           context->storage->InternString("ANDROID_WEB_VIEW"),
           context->storage->InternString("NACL_HOST"),
           context->storage->InternString("ENCRYPTED_MEDIA"),
           context->storage->InternString("CAST"),
           context->storage->InternString("GIN_JAVA_BRIDGE"),
           context->storage->InternString("CHROME_UTILITY_PRINTING"),
           context->storage->InternString("OZONE_GPU"),
           context->storage->InternString("WEB_TEST"),
           context->storage->InternString("NETWORK_HINTS"),
           context->storage->InternString("EXTENSIONS_GUEST_VIEW"),
           context->storage->InternString("GUEST_VIEW"),
           context->storage->InternString("MEDIA_PLAYER_DELEGATE"),
           context->storage->InternString("EXTENSION_WORKER"),
           context->storage->InternString("SUBRESOURCE_FILTER"),
           context->storage->InternString("UNFREEZABLE_FRAME")}} {}

void TrackEventParser::ParseTrackEvent(int64_t ts,
                                       int64_t tts,
                                       int64_t ticount,
                                       PacketSequenceState* sequence_state,
                                       size_t sequence_state_generation,
                                       ConstBytes blob) {
  using LegacyEvent = protos::pbzero::TrackEvent::LegacyEvent;

  protos::pbzero::TrackEvent::Decoder event(blob.data, blob.size);

  const auto legacy_event_blob = event.legacy_event();
  LegacyEvent::Decoder legacy_event(legacy_event_blob.data,
                                    legacy_event_blob.size);

  // TODO(eseckler): This legacy event field will eventually be replaced by
  // fields in TrackEvent itself.
  if (PERFETTO_UNLIKELY(!event.type() && !legacy_event.has_phase())) {
    context_->storage->IncrementStats(stats::track_event_parser_errors);
    PERFETTO_DLOG("TrackEvent without type or phase");
    return;
  }

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  TrackTracker* track_tracker = context_->track_tracker.get();
  SliceTracker* slice_tracker = context_->slice_tracker.get();

  std::vector<uint64_t> category_iids;
  for (auto it = event.category_iids(); it; ++it) {
    category_iids.push_back(*it);
  }
  std::vector<protozero::ConstChars> category_strings;
  for (auto it = event.categories(); it; ++it) {
    category_strings.push_back(*it);
  }

  StringId category_id = 0;

  // If there's a single category, we can avoid building a concatenated
  // string.
  if (PERFETTO_LIKELY(category_iids.size() == 1 && category_strings.empty())) {
    auto* decoder = sequence_state->LookupInternedMessage<
        protos::pbzero::InternedData::kEventCategoriesFieldNumber,
        protos::pbzero::EventCategory>(sequence_state_generation,
                                       category_iids[0]);
    if (decoder)
      category_id = storage->InternString(decoder->name());
  } else if (category_iids.empty() && category_strings.size() == 1) {
    category_id = storage->InternString(category_strings[0]);
  } else if (category_iids.size() + category_strings.size() > 1) {
    // We concatenate the category strings together since we currently only
    // support a single "cat" column.
    // TODO(eseckler): Support multi-category events in the table schema.
    std::string categories;
    for (uint64_t iid : category_iids) {
      auto* decoder = sequence_state->LookupInternedMessage<
          protos::pbzero::InternedData::kEventCategoriesFieldNumber,
          protos::pbzero::EventCategory>(sequence_state_generation, iid);
      if (!decoder)
        continue;
      base::StringView name = decoder->name();
      if (!categories.empty())
        categories.append(",");
      categories.append(name.data(), name.size());
    }
    for (const protozero::ConstChars& cat : category_strings) {
      if (!categories.empty())
        categories.append(",");
      categories.append(cat.data, cat.size);
    }
    if (!categories.empty())
      category_id = storage->InternString(base::StringView(categories));
  }

  StringId name_id = 0;

  uint64_t name_iid = event.name_iid();
  if (!name_iid)
    name_iid = legacy_event.name_iid();

  if (PERFETTO_LIKELY(name_iid)) {
    auto* decoder = sequence_state->LookupInternedMessage<
        protos::pbzero::InternedData::kEventNamesFieldNumber,
        protos::pbzero::EventName>(sequence_state_generation, name_iid);
    if (decoder)
      name_id = storage->InternString(decoder->name());
  } else if (event.has_name()) {
    name_id = storage->InternString(event.name());
  }

  // TODO(eseckler): Also consider track_uuid from TrackEventDefaults.
  // Fall back to the default descriptor track (uuid 0).
  uint64_t track_uuid = event.has_track_uuid() ? event.track_uuid() : 0u;
  TrackId track_id;
  base::Optional<UniqueTid> utid;
  base::Optional<UniqueTid> upid;

  // Determine track from track_uuid specified in either TrackEvent or
  // TrackEventDefaults. If none is set, fall back to the track specified by the
  // sequence's (or event's) pid + tid or a default track.
  if (track_uuid) {
    base::Optional<TrackId> opt_track_id =
        track_tracker->GetDescriptorTrack(track_uuid);
    if (!opt_track_id) {
      storage->IncrementStats(stats::track_event_parser_errors);
      PERFETTO_DLOG("TrackEvent with unknown track_uuid %" PRIu64, track_uuid);
      return;
    }
    track_id = *opt_track_id;

    auto thread_track_row =
        context_->storage->thread_track_table().id().IndexOf(
            SqlValue::Long(track_id));
    if (thread_track_row) {
      utid = storage->thread_track_table().utid()[*thread_track_row];
      upid = storage->GetThread(*utid).upid;
    } else {
      auto process_track_row =
          context_->storage->process_track_table().id().IndexOf(
              SqlValue::Long(track_id));
      if (process_track_row)
        upid = storage->process_track_table().upid()[*process_track_row];
    }
  } else if (sequence_state->pid_and_tid_valid() ||
             (legacy_event.has_pid_override() &&
              legacy_event.has_tid_override())) {
    uint32_t pid = static_cast<uint32_t>(sequence_state->pid());
    uint32_t tid = static_cast<uint32_t>(sequence_state->tid());
    if (legacy_event.has_pid_override())
      pid = static_cast<uint32_t>(legacy_event.pid_override());
    if (legacy_event.has_tid_override())
      tid = static_cast<uint32_t>(legacy_event.tid_override());

    utid = procs->UpdateThread(tid, pid);
    upid = storage->GetThread(*utid).upid;
    track_id = track_tracker->GetOrCreateDescriptorTrackForThread(*utid);
  } else {
    track_id = track_tracker->GetOrCreateDefaultDescriptorTrack();
  }

  // All events in legacy JSON require a thread ID, but for some types of events
  // (e.g. async events or process/global-scoped instants), we don't store it in
  // the slice/track model. To pass the original tid through to the json export,
  // we store it in an arg.
  uint32_t legacy_tid = 0;

  // TODO(eseckler): Replace phase with type and remove handling of
  // legacy_event.phase() once it is no longer used by producers.
  int32_t phase = 0;
  if (legacy_event.has_phase()) {
    phase = legacy_event.phase();

    switch (phase) {
      case 'b':
      case 'e':
      case 'n': {
        // Intern tracks for legacy async events based on legacy event ids.
        int64_t source_id = 0;
        bool source_id_is_process_scoped = false;
        if (legacy_event.has_unscoped_id()) {
          source_id = static_cast<int64_t>(legacy_event.unscoped_id());
        } else if (legacy_event.has_global_id()) {
          source_id = static_cast<int64_t>(legacy_event.global_id());
        } else if (legacy_event.has_local_id()) {
          if (!upid) {
            storage->IncrementStats(stats::track_event_parser_errors);
            PERFETTO_DLOG(
                "TrackEvent with local_id without process association");
            return;
          }

          source_id = static_cast<int64_t>(legacy_event.local_id());
          source_id_is_process_scoped = true;
        } else {
          storage->IncrementStats(stats::track_event_parser_errors);
          PERFETTO_DLOG("Async LegacyEvent without ID");
          return;
        }

        // Catapult treats nestable async events of different categories with
        // the same ID as separate tracks. We replicate the same behavior here.
        StringId id_scope = category_id;
        if (legacy_event.has_id_scope()) {
          std::string concat = storage->GetString(category_id).ToStdString() +
                               ":" + legacy_event.id_scope().ToStdString();
          id_scope = storage->InternString(base::StringView(concat));
        }

        track_id = context_->track_tracker->InternLegacyChromeAsyncTrack(
            name_id, upid ? *upid : 0, source_id, source_id_is_process_scoped,
            id_scope);
        if (utid)
          legacy_tid = storage->GetThread(*utid).tid;
        break;
      }
      case 'i':
      case 'I': {
        // Intern tracks for global or process-scoped legacy instant events.
        switch (legacy_event.instant_event_scope()) {
          case LegacyEvent::SCOPE_UNSPECIFIED:
          case LegacyEvent::SCOPE_THREAD:
            // Thread-scoped legacy instant events already have the right track
            // based on the tid/pid of the sequence.
            if (!utid) {
              storage->IncrementStats(stats::track_event_parser_errors);
              PERFETTO_DLOG(
                  "Thread-scoped instant event without thread association");
              return;
            }
            break;
          case LegacyEvent::SCOPE_GLOBAL:
            track_id = context_->track_tracker
                           ->GetOrCreateLegacyChromeGlobalInstantTrack();
            if (utid)
              legacy_tid = storage->GetThread(*utid).tid;
            break;
          case LegacyEvent::SCOPE_PROCESS:
            if (!upid) {
              storage->IncrementStats(stats::track_event_parser_errors);
              PERFETTO_DLOG(
                  "Process-scoped instant event without process association");
              return;
            }

            track_id =
                context_->track_tracker->InternLegacyChromeProcessInstantTrack(
                    *upid);
            if (utid)
              legacy_tid = storage->GetThread(*utid).tid;
            break;
        }
        break;
      }
      default:
        break;
    }
  } else {
    switch (event.type()) {
      case protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN:
        phase = utid ? 'B' : 'b';
        break;
      case protos::pbzero::TrackEvent::TYPE_SLICE_END:
        phase = utid ? 'E' : 'e';
        break;
      case protos::pbzero::TrackEvent::TYPE_INSTANT:
        phase = utid ? 'i' : 'n';
        break;
      default:
        PERFETTO_FATAL("unexpected event type %d", event.type());
        return;
    }
  }

  auto args_callback = [this, &event, &legacy_event, &sequence_state,
                        sequence_state_generation, ts, utid,
                        legacy_tid](ArgsTracker* args_tracker, RowId row_id) {
    for (auto it = event.debug_annotations(); it; ++it) {
      ParseDebugAnnotationArgs(*it, sequence_state, sequence_state_generation,
                               args_tracker, row_id);
    }

    if (event.has_task_execution()) {
      ParseTaskExecutionArgs(event.task_execution(), sequence_state,
                             sequence_state_generation, args_tracker, row_id);
    }

    if (event.has_log_message()) {
      ParseLogMessage(event.log_message(), sequence_state,
                      sequence_state_generation, ts, utid, args_tracker,
                      row_id);
    }
    if (event.has_cc_scheduler_state()) {
      ParseCcScheduler(event.cc_scheduler_state(), sequence_state,
                       sequence_state_generation, args_tracker, row_id);
    }
    if (event.has_chrome_user_event()) {
      ParseChromeUserEvent(event.chrome_user_event(), args_tracker, row_id);
    }
    if (event.has_chrome_legacy_ipc()) {
      ParseChromeLegacyIpc(event.chrome_legacy_ipc(), args_tracker, row_id);
    }
    if (event.has_chrome_keyed_service()) {
      ParseChromeKeyedService(event.chrome_keyed_service(), args_tracker,
                              row_id);
    }

    if (legacy_tid) {
      args_tracker->AddArg(row_id, legacy_event_original_tid_id_,
                           legacy_event_original_tid_id_,
                           Variadic::Integer(static_cast<int32_t>(legacy_tid)));
    }

    // TODO(eseckler): Parse legacy flow events into flow events table once we
    // have a design for it.
    if (legacy_event.has_bind_id()) {
      args_tracker->AddArg(row_id, legacy_event_bind_id_key_id_,
                           legacy_event_bind_id_key_id_,
                           Variadic::UnsignedInteger(legacy_event.bind_id()));
    }

    if (legacy_event.bind_to_enclosing()) {
      args_tracker->AddArg(row_id, legacy_event_bind_to_enclosing_key_id_,
                           legacy_event_bind_to_enclosing_key_id_,
                           Variadic::Boolean(true));
    }

    if (legacy_event.flow_direction()) {
      StringId value;
      switch (legacy_event.flow_direction()) {
        case protos::pbzero::TrackEvent::LegacyEvent::FLOW_IN:
          value = flow_direction_value_in_id_;
          break;
        case protos::pbzero::TrackEvent::LegacyEvent::FLOW_OUT:
          value = flow_direction_value_out_id_;
          break;
        case protos::pbzero::TrackEvent::LegacyEvent::FLOW_INOUT:
          value = flow_direction_value_inout_id_;
          break;
        default:
          PERFETTO_FATAL("Unknown flow direction: %d",
                         legacy_event.flow_direction());
          break;
      }
      args_tracker->AddArg(row_id, legacy_event_flow_direction_key_id_,
                           legacy_event_flow_direction_key_id_,
                           Variadic::String(value));
    }
  };

  switch (static_cast<char>(phase)) {
    case 'B': {  // TRACE_EVENT_PHASE_BEGIN.
      if (!utid) {
        storage->IncrementStats(stats::track_event_parser_errors);
        PERFETTO_DLOG("TrackEvent with phase B without thread association");
        return;
      }

      auto opt_slice_id =
          slice_tracker->Begin(ts, track_id, *utid, RefType::kRefUtid,
                               category_id, name_id, args_callback);
      if (opt_slice_id.has_value()) {
        auto* thread_slices = storage->mutable_thread_slices();
        PERFETTO_DCHECK(!thread_slices->slice_count() ||
                        thread_slices->slice_ids().back() <
                            opt_slice_id.value());
        thread_slices->AddThreadSlice(opt_slice_id.value(), tts,
                                      kPendingThreadDuration, ticount,
                                      kPendingThreadInstructionDelta);
      }
      break;
    }
    case 'E': {  // TRACE_EVENT_PHASE_END.
      if (!utid) {
        storage->IncrementStats(stats::track_event_parser_errors);
        PERFETTO_DLOG("TrackEvent with phase E without thread association");
        return;
      }

      auto opt_slice_id =
          slice_tracker->End(ts, track_id, category_id, name_id, args_callback);
      if (opt_slice_id.has_value()) {
        auto* thread_slices = storage->mutable_thread_slices();
        thread_slices->UpdateThreadDeltasForSliceId(opt_slice_id.value(), tts,
                                                    ticount);
      }
      break;
    }
    case 'X': {  // TRACE_EVENT_PHASE_COMPLETE.
      if (!utid) {
        storage->IncrementStats(stats::track_event_parser_errors);
        PERFETTO_DLOG("TrackEvent with phase X without thread association");
        return;
      }

      auto duration_ns = legacy_event.duration_us() * 1000;
      if (duration_ns < 0)
        return;
      auto opt_slice_id = slice_tracker->Scoped(
          ts, track_id, *utid, RefType::kRefUtid, category_id, name_id,
          duration_ns, args_callback);
      if (opt_slice_id.has_value()) {
        auto* thread_slices = storage->mutable_thread_slices();
        PERFETTO_DCHECK(!thread_slices->slice_count() ||
                        thread_slices->slice_ids().back() <
                            opt_slice_id.value());
        auto thread_duration_ns = legacy_event.thread_duration_us() * 1000;
        thread_slices->AddThreadSlice(opt_slice_id.value(), tts,
                                      thread_duration_ns, ticount,
                                      legacy_event.thread_instruction_delta());
      }
      break;
    }
    case 'i':
    case 'I': {  // TRACE_EVENT_PHASE_INSTANT.
      // Handle instant events as slices with zero duration, so that they end
      // up nested underneath their parent slices.
      int64_t duration_ns = 0;
      int64_t tidelta = 0;

      switch (legacy_event.instant_event_scope()) {
        case LegacyEvent::SCOPE_UNSPECIFIED:
        case LegacyEvent::SCOPE_THREAD: {
          // TODO(lalitm): Associate thread slices with track instead.
          auto opt_slice_id = slice_tracker->Scoped(
              ts, track_id, *utid, RefType::kRefUtid, category_id, name_id,
              duration_ns, args_callback);
          if (opt_slice_id.has_value()) {
            auto* thread_slices = storage->mutable_thread_slices();
            PERFETTO_DCHECK(!thread_slices->slice_count() ||
                            thread_slices->slice_ids().back() <
                                opt_slice_id.value());
            thread_slices->AddThreadSlice(opt_slice_id.value(), tts,
                                          duration_ns, ticount, tidelta);
          }
          break;
        }
        case LegacyEvent::SCOPE_GLOBAL: {
          slice_tracker->Scoped(ts, track_id, /*ref=*/0, RefType::kRefNoRef,
                                category_id, name_id, duration_ns,
                                args_callback);
          break;
        }
        case LegacyEvent::SCOPE_PROCESS: {
          slice_tracker->Scoped(ts, track_id, *upid, RefType::kRefUpid,
                                category_id, name_id, duration_ns,
                                args_callback);
          break;
        }
        default: {
          PERFETTO_FATAL("Unknown instant event scope: %u",
                         legacy_event.instant_event_scope());
          break;
        }
      }
      break;
    }
    case 'b': {  // TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN
      auto opt_slice_id =
          slice_tracker->Begin(ts, track_id, track_id, RefType::kRefTrack,
                               category_id, name_id, args_callback);
      // For the time beeing, we only create vtrack slice rows if we need to
      // store thread timestamps/counters.
      if (legacy_event.use_async_tts() && opt_slice_id.has_value()) {
        auto* vtrack_slices = storage->mutable_virtual_track_slices();
        PERFETTO_DCHECK(!vtrack_slices->slice_count() ||
                        vtrack_slices->slice_ids().back() <
                            opt_slice_id.value());
        vtrack_slices->AddVirtualTrackSlice(opt_slice_id.value(), tts,
                                            kPendingThreadDuration, ticount,
                                            kPendingThreadInstructionDelta);
      }
      break;
    }
    case 'e': {  // TRACE_EVENT_PHASE_NESTABLE_ASYNC_END
      auto opt_slice_id =
          slice_tracker->End(ts, track_id, category_id, name_id, args_callback);
      if (legacy_event.use_async_tts() && opt_slice_id.has_value()) {
        auto* vtrack_slices = storage->mutable_virtual_track_slices();
        vtrack_slices->UpdateThreadDeltasForSliceId(opt_slice_id.value(), tts,
                                                    ticount);
      }
      break;
    }
    case 'n': {  // TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT
      // Handle instant events as slices with zero duration, so that they end up
      // nested underneath their parent slices.
      int64_t duration_ns = 0;
      int64_t tidelta = 0;
      auto opt_slice_id = slice_tracker->Scoped(
          ts, track_id, track_id, RefType::kRefTrack, category_id, name_id,
          duration_ns, args_callback);
      if (legacy_event.use_async_tts() && opt_slice_id.has_value()) {
        auto* vtrack_slices = storage->mutable_virtual_track_slices();
        PERFETTO_DCHECK(!vtrack_slices->slice_count() ||
                        vtrack_slices->slice_ids().back() <
                            opt_slice_id.value());
        vtrack_slices->AddVirtualTrackSlice(opt_slice_id.value(), tts,
                                            duration_ns, ticount, tidelta);
      }
      break;
    }
    case 'M': {  // TRACE_EVENT_PHASE_METADATA (process and thread names).
      // Parse process and thread names from correspondingly named events.
      // TODO(eseckler): Also consider names from process/thread descriptors.
      NullTermStringView event_name = storage->GetString(name_id);
      PERFETTO_DCHECK(event_name.data());
      if (strcmp(event_name.c_str(), "thread_name") == 0) {
        if (!utid) {
          storage->IncrementStats(stats::track_event_parser_errors);
          PERFETTO_DLOG(
              "thread_name metadata event without thread association");
          return;
        }

        auto it = event.debug_annotations();
        if (!it)
          break;
        protos::pbzero::DebugAnnotation::Decoder annotation(*it);
        auto thread_name = annotation.string_value();
        if (!thread_name.size)
          break;
        auto thread_name_id = storage->InternString(thread_name);
        // Don't override system-provided names.
        procs->SetThreadNameIfUnset(*utid, thread_name_id);
        break;
      }
      if (strcmp(event_name.c_str(), "process_name") == 0) {
        if (!upid) {
          storage->IncrementStats(stats::track_event_parser_errors);
          PERFETTO_DLOG(
              "process_name metadata event without process association");
          return;
        }

        auto it = event.debug_annotations();
        if (!it)
          break;
        protos::pbzero::DebugAnnotation::Decoder annotation(*it);
        auto process_name = annotation.string_value();
        if (!process_name.size)
          break;
        auto process_name_id = storage->InternString(process_name);
        // Don't override system-provided names.
        procs->SetProcessNameIfUnset(*upid, process_name_id);
        break;
      }
      // Other metadata events are proxied via the raw table for JSON export.
      ParseLegacyEventAsRawEvent(ts, tts, ticount, utid, category_id, name_id,
                                 legacy_event, args_callback);
      break;
    }
    default: {
      // Other events are proxied via the raw table for JSON export.
      ParseLegacyEventAsRawEvent(ts, tts, ticount, utid, category_id, name_id,
                                 legacy_event, args_callback);
    }
  }
}

void TrackEventParser::ParseLegacyEventAsRawEvent(
    int64_t ts,
    int64_t tts,
    int64_t ticount,
    base::Optional<UniqueTid> utid,
    StringId category_id,
    StringId name_id,
    const protos::pbzero::TrackEvent::LegacyEvent::Decoder& legacy_event,
    SliceTracker::SetArgsCallback args_callback) {
  if (!utid) {
    context_->storage->IncrementStats(stats::track_event_parser_errors);
    PERFETTO_DLOG("raw legacy event without thread association");
    return;
  }

  RowId row_id = context_->storage->mutable_raw_events()->AddRawEvent(
      ts, raw_legacy_event_id_, 0, *utid);
  ArgsTracker args(context_);
  args.AddArg(row_id, legacy_event_category_key_id_,
              legacy_event_category_key_id_, Variadic::String(category_id));
  args.AddArg(row_id, legacy_event_name_key_id_, legacy_event_name_key_id_,
              Variadic::String(name_id));

  std::string phase_string(1, static_cast<char>(legacy_event.phase()));
  StringId phase_id = context_->storage->InternString(phase_string.c_str());
  args.AddArg(row_id, legacy_event_phase_key_id_, legacy_event_phase_key_id_,
              Variadic::String(phase_id));

  if (legacy_event.has_duration_us()) {
    args.AddArg(row_id, legacy_event_duration_ns_key_id_,
                legacy_event_duration_ns_key_id_,
                Variadic::Integer(legacy_event.duration_us() * 1000));
  }

  if (tts) {
    args.AddArg(row_id, legacy_event_thread_timestamp_ns_key_id_,
                legacy_event_thread_timestamp_ns_key_id_,
                Variadic::Integer(tts));
    if (legacy_event.has_thread_duration_us()) {
      args.AddArg(row_id, legacy_event_thread_duration_ns_key_id_,
                  legacy_event_thread_duration_ns_key_id_,
                  Variadic::Integer(legacy_event.thread_duration_us() * 1000));
    }
  }

  if (ticount) {
    args.AddArg(row_id, legacy_event_thread_instruction_count_key_id_,
                legacy_event_thread_instruction_count_key_id_,
                Variadic::Integer(tts));
    if (legacy_event.has_thread_instruction_delta()) {
      args.AddArg(row_id, legacy_event_thread_instruction_delta_key_id_,
                  legacy_event_thread_instruction_delta_key_id_,
                  Variadic::Integer(legacy_event.thread_instruction_delta()));
    }
  }

  if (legacy_event.use_async_tts()) {
    args.AddArg(row_id, legacy_event_use_async_tts_key_id_,
                legacy_event_use_async_tts_key_id_, Variadic::Boolean(true));
  }

  bool has_id = false;
  if (legacy_event.has_unscoped_id()) {
    // Unscoped ids are either global or local depending on the phase. Pass them
    // through as unscoped IDs to JSON export to preserve this behavior.
    args.AddArg(row_id, legacy_event_unscoped_id_key_id_,
                legacy_event_unscoped_id_key_id_,
                Variadic::UnsignedInteger(legacy_event.unscoped_id()));
    has_id = true;
  } else if (legacy_event.has_global_id()) {
    args.AddArg(row_id, legacy_event_global_id_key_id_,
                legacy_event_global_id_key_id_,
                Variadic::UnsignedInteger(legacy_event.global_id()));
    has_id = true;
  } else if (legacy_event.has_local_id()) {
    args.AddArg(row_id, legacy_event_local_id_key_id_,
                legacy_event_local_id_key_id_,
                Variadic::UnsignedInteger(legacy_event.local_id()));
    has_id = true;
  }

  if (has_id && legacy_event.has_id_scope() && legacy_event.id_scope().size) {
    args.AddArg(row_id, legacy_event_id_scope_key_id_,
                legacy_event_id_scope_key_id_,
                Variadic::String(
                    context_->storage->InternString(legacy_event.id_scope())));
  }

  // No need to parse legacy_event.instant_event_scope() because we import
  // instant events into the slice table.

  args_callback(&args, row_id);
}

void TrackEventParser::ParseDebugAnnotationArgs(
    ConstBytes debug_annotation,
    PacketSequenceState* sequence_state,
    size_t sequence_state_generation,
    ArgsTracker* args_tracker,
    RowId row_id) {
  TraceStorage* storage = context_->storage.get();

  protos::pbzero::DebugAnnotation::Decoder annotation(debug_annotation.data,
                                                      debug_annotation.size);

  StringId name_id = 0;

  uint64_t name_iid = annotation.name_iid();
  if (PERFETTO_LIKELY(name_iid)) {
    auto* decoder = sequence_state->LookupInternedMessage<
        protos::pbzero::InternedData::kDebugAnnotationNamesFieldNumber,
        protos::pbzero::DebugAnnotationName>(sequence_state_generation,
                                             name_iid);
    if (!decoder)
      return;

    std::string name_prefixed = "debug." + decoder->name().ToStdString();
    name_id = storage->InternString(base::StringView(name_prefixed));
  } else if (annotation.has_name()) {
    name_id = storage->InternString(annotation.name());
  } else {
    context_->storage->IncrementStats(stats::track_event_parser_errors);
    PERFETTO_DLOG("Debug annotation without name");
    return;
  }

  if (annotation.has_bool_value()) {
    args_tracker->AddArg(row_id, name_id, name_id,
                         Variadic::Boolean(annotation.bool_value()));
  } else if (annotation.has_uint_value()) {
    args_tracker->AddArg(row_id, name_id, name_id,
                         Variadic::UnsignedInteger(annotation.uint_value()));
  } else if (annotation.has_int_value()) {
    args_tracker->AddArg(row_id, name_id, name_id,
                         Variadic::Integer(annotation.int_value()));
  } else if (annotation.has_double_value()) {
    args_tracker->AddArg(row_id, name_id, name_id,
                         Variadic::Real(annotation.double_value()));
  } else if (annotation.has_string_value()) {
    args_tracker->AddArg(
        row_id, name_id, name_id,
        Variadic::String(storage->InternString(annotation.string_value())));
  } else if (annotation.has_pointer_value()) {
    args_tracker->AddArg(row_id, name_id, name_id,
                         Variadic::Pointer(annotation.pointer_value()));
  } else if (annotation.has_legacy_json_value()) {
    args_tracker->AddArg(
        row_id, name_id, name_id,
        Variadic::Json(storage->InternString(annotation.legacy_json_value())));
  } else if (annotation.has_nested_value()) {
    auto name = storage->GetString(name_id);
    ParseNestedValueArgs(annotation.nested_value(), name, name, args_tracker,
                         row_id);
  }
}

void TrackEventParser::ParseNestedValueArgs(ConstBytes nested_value,
                                            base::StringView flat_key,
                                            base::StringView key,
                                            ArgsTracker* args_tracker,
                                            RowId row_id) {
  protos::pbzero::DebugAnnotation::NestedValue::Decoder value(
      nested_value.data, nested_value.size);
  switch (value.nested_type()) {
    case protos::pbzero::DebugAnnotation::NestedValue::UNSPECIFIED: {
      auto flat_key_id = context_->storage->InternString(flat_key);
      auto key_id = context_->storage->InternString(key);
      // Leaf value.
      if (value.has_bool_value()) {
        args_tracker->AddArg(row_id, flat_key_id, key_id,
                             Variadic::Boolean(value.bool_value()));
      } else if (value.has_int_value()) {
        args_tracker->AddArg(row_id, flat_key_id, key_id,
                             Variadic::Integer(value.int_value()));
      } else if (value.has_double_value()) {
        args_tracker->AddArg(row_id, flat_key_id, key_id,
                             Variadic::Real(value.double_value()));
      } else if (value.has_string_value()) {
        args_tracker->AddArg(row_id, flat_key_id, key_id,
                             Variadic::String(context_->storage->InternString(
                                 value.string_value())));
      }
      break;
    }
    case protos::pbzero::DebugAnnotation::NestedValue::DICT: {
      auto key_it = value.dict_keys();
      auto value_it = value.dict_values();
      for (; key_it && value_it; ++key_it, ++value_it) {
        std::string child_name = (*key_it).ToStdString();
        std::string child_flat_key = flat_key.ToStdString() + "." + child_name;
        std::string child_key = key.ToStdString() + "." + child_name;
        ParseNestedValueArgs(*value_it, base::StringView(child_flat_key),
                             base::StringView(child_key), args_tracker, row_id);
      }
      break;
    }
    case protos::pbzero::DebugAnnotation::NestedValue::ARRAY: {
      int child_index = 0;
      std::string child_flat_key = flat_key.ToStdString();
      for (auto value_it = value.array_values(); value_it;
           ++value_it, ++child_index) {
        std::string child_key =
            key.ToStdString() + "[" + std::to_string(child_index) + "]";
        ParseNestedValueArgs(*value_it, base::StringView(child_flat_key),
                             base::StringView(child_key), args_tracker, row_id);
      }
      break;
    }
  }
}

void TrackEventParser::ParseTaskExecutionArgs(
    ConstBytes task_execution,
    PacketSequenceState* sequence_state,
    size_t sequence_state_generation,
    ArgsTracker* args_tracker,
    RowId row) {
  protos::pbzero::TaskExecution::Decoder task(task_execution.data,
                                              task_execution.size);
  uint64_t iid = task.posted_from_iid();
  if (!iid)
    return;

  auto* decoder = sequence_state->LookupInternedMessage<
      protos::pbzero::InternedData::kSourceLocationsFieldNumber,
      protos::pbzero::SourceLocation>(sequence_state_generation, iid);
  if (!decoder)
    return;

  StringId file_name_id = 0;
  StringId function_name_id = 0;
  uint32_t line_number = 0;

  TraceStorage* storage = context_->storage.get();
  file_name_id = storage->InternString(decoder->file_name());
  function_name_id = storage->InternString(decoder->function_name());
  line_number = decoder->line_number();

  args_tracker->AddArg(row, task_file_name_args_key_id_,
                       task_file_name_args_key_id_,
                       Variadic::String(file_name_id));
  args_tracker->AddArg(row, task_function_name_args_key_id_,
                       task_function_name_args_key_id_,
                       Variadic::String(function_name_id));

  args_tracker->AddArg(row, task_line_number_args_key_id_,
                       task_line_number_args_key_id_,
                       Variadic::UnsignedInteger(line_number));
}

void TrackEventParser::ParseLogMessage(ConstBytes blob,
                                       PacketSequenceState* sequence_state,
                                       size_t sequence_state_generation,
                                       int64_t ts,
                                       base::Optional<UniqueTid> utid,
                                       ArgsTracker* args_tracker,
                                       RowId row) {
  if (!utid) {
    context_->storage->IncrementStats(stats::track_event_parser_errors);
    PERFETTO_DLOG("LogMessage without thread association");
    return;
  }

  protos::pbzero::LogMessage::Decoder message(blob.data, blob.size);

  TraceStorage* storage = context_->storage.get();

  StringId log_message_id = 0;

  auto* decoder = sequence_state->LookupInternedMessage<
      protos::pbzero::InternedData::kLogMessageBodyFieldNumber,
      protos::pbzero::LogMessageBody>(sequence_state_generation,
                                      message.body_iid());
  if (!decoder)
    return;

  log_message_id = storage->InternString(decoder->body());

  // TODO(nicomazz): LogMessage also contains the source of the message (file
  // and line number). Android logs doesn't support this so far.
  context_->storage->mutable_android_log()->AddLogEvent(
      ts, *utid,
      /*priority*/ 0,
      /*tag_id*/ 0,  // TODO(nicomazz): Abuse tag_id to display
                     // "file_name:line_number".
      log_message_id);

  args_tracker->AddArg(row, log_message_body_key_id_, log_message_body_key_id_,
                       Variadic::String(log_message_id));
  // TODO(nicomazz): Add the source location as an argument.
}

void TrackEventParser::ParseCcScheduler(ConstBytes cc,
                                        PacketSequenceState* sequence_state,
                                        size_t sequence_state_generation,
                                        ArgsTracker* args_tracker,
                                        RowId row) {
  // The 79 decides the initial amount of memory reserved in the prefix. This
  // was determined my manually counting the length of the longest column.
  constexpr size_t kCcSchedulerStateMaxColumnLength = 79;
  ProtoToArgsTable helper(
      sequence_state, sequence_state_generation, context_, args_tracker,
      /* starting_prefix = */ "", kCcSchedulerStateMaxColumnLength);
  auto status = helper.AddProtoFileDescriptor(
      kChromeCompositorSchedulerStateDescriptor.data(),
      kChromeCompositorSchedulerStateDescriptor.size());
  PERFETTO_DCHECK(status.ok());

  // Switch |source_location_iid| into its interned data variant.
  helper.AddParsingOverride(
      "begin_impl_frame_args.current_args.source_location_iid",
      [](const ProtoToArgsTable::ParsingOverrideState& state,
         const protozero::Field& field) {
        return MaybeParseSourceLocation("begin_impl_frame_args.current_args",
                                        state, field);
      });
  helper.AddParsingOverride(
      "begin_impl_frame_args.last_args.source_location_iid",
      [](const ProtoToArgsTable::ParsingOverrideState& state,
         const protozero::Field& field) {
        return MaybeParseSourceLocation("begin_impl_frame_args.last_args",
                                        state, field);
      });
  helper.AddParsingOverride(
      "begin_frame_observer_state.last_begin_frame_args.source_location_iid",
      [](const ProtoToArgsTable::ParsingOverrideState& state,
         const protozero::Field& field) {
        return MaybeParseSourceLocation(
            "begin_frame_observer_state.last_begin_frame_args", state, field);
      });
  helper.InternProtoIntoArgsTable(
      cc, ".perfetto.protos.ChromeCompositorSchedulerState", row);
}

void TrackEventParser::ParseChromeUserEvent(
    protozero::ConstBytes chrome_user_event,
    ArgsTracker* args_tracker,
    RowId row) {
  protos::pbzero::ChromeUserEvent::Decoder event(chrome_user_event.data,
                                                 chrome_user_event.size);
  if (event.has_action()) {
    StringId action_id = context_->storage->InternString(event.action());
    args_tracker->AddArg(row, chrome_user_event_action_args_key_id_,
                         chrome_user_event_action_args_key_id_,
                         Variadic::String(action_id));
  }
}

void TrackEventParser::ParseChromeLegacyIpc(
    protozero::ConstBytes chrome_legacy_ipc,
    ArgsTracker* args_tracker,
    RowId row) {
  protos::pbzero::ChromeLegacyIpc::Decoder event(chrome_legacy_ipc.data,
                                                 chrome_legacy_ipc.size);
  if (event.has_message_class()) {
    size_t message_class_index = static_cast<size_t>(event.message_class());
    if (message_class_index >= chrome_legacy_ipc_class_ids_.size())
      message_class_index = 0;
    args_tracker->AddArg(
        row, chrome_legacy_ipc_class_args_key_id_,
        chrome_legacy_ipc_class_args_key_id_,
        Variadic::String(chrome_legacy_ipc_class_ids_[message_class_index]));
  }
  if (event.has_message_line()) {
    args_tracker->AddArg(row, chrome_legacy_ipc_line_args_key_id_,
                         chrome_legacy_ipc_line_args_key_id_,
                         Variadic::Integer(event.message_line()));
  }
}

void TrackEventParser::ParseChromeKeyedService(
    protozero::ConstBytes chrome_keyed_service,
    ArgsTracker* args_tracker,
    RowId row) {
  protos::pbzero::ChromeKeyedService::Decoder event(chrome_keyed_service.data,
                                                    chrome_keyed_service.size);
  if (event.has_name()) {
    StringId action_id = context_->storage->InternString(event.name());
    args_tracker->AddArg(row, chrome_keyed_service_name_args_key_id_,
                         chrome_keyed_service_name_args_key_id_,
                         Variadic::String(action_id));
  }
}

}  // namespace trace_processor
}  // namespace perfetto
