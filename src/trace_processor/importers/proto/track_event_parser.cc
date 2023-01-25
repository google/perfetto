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

#include <iostream>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_writer.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/proto/packet_analyzer.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/profile_packet_utils.h"
#include "src/trace_processor/importers/proto/track_event_tracker.h"
#include "src/trace_processor/util/debug_annotation_parser.h"
#include "src/trace_processor/util/proto_to_args_parser.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/trace/extension_descriptor.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_active_processes.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_compositor_scheduler_state.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_histogram_sample.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_legacy_ipc.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/log_message.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "protos/perfetto/trace/track_event/task_execution.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
using BoundInserter = ArgsTracker::BoundInserter;
using protos::pbzero::TrackEvent;
using LegacyEvent = TrackEvent::LegacyEvent;
using protozero::ConstBytes;

// Slices which have been opened but haven't been closed yet will be marked
// with these placeholder values.
constexpr int64_t kPendingThreadDuration = -1;
constexpr int64_t kPendingThreadInstructionDelta = -1;

class TrackEventArgsParser : public util::ProtoToArgsParser::Delegate {
 public:
  TrackEventArgsParser(int64_t packet_timestamp,
                       BoundInserter& inserter,
                       TraceStorage& storage,
                       PacketSequenceStateGeneration& sequence_state)
      : packet_timestamp_(packet_timestamp),
        inserter_(inserter),
        storage_(storage),
        sequence_state_(sequence_state) {}

  ~TrackEventArgsParser() override;

  using Key = util::ProtoToArgsParser::Key;

  void AddInteger(const Key& key, int64_t value) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::Integer(value));
  }
  void AddUnsignedInteger(const Key& key, uint64_t value) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::UnsignedInteger(value));
  }
  void AddString(const Key& key, const protozero::ConstChars& value) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::String(storage_.InternString(value)));
  }
  void AddString(const Key& key, const std::string& value) final {
    inserter_.AddArg(
        storage_.InternString(base::StringView(key.flat_key)),
        storage_.InternString(base::StringView(key.key)),
        Variadic::String(storage_.InternString(base::StringView(value))));
  }
  void AddDouble(const Key& key, double value) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::Real(value));
  }
  void AddPointer(const Key& key, const void* value) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::Pointer(reinterpret_cast<uintptr_t>(value)));
  }
  void AddBoolean(const Key& key, bool value) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::Boolean(value));
  }
  bool AddJson(const Key& key, const protozero::ConstChars& value) final {
    auto json_value = json::ParseJsonString(value);
    if (!json_value)
      return false;
    return json::AddJsonValueToArgs(*json_value, base::StringView(key.flat_key),
                                    base::StringView(key.key), &storage_,
                                    &inserter_);
  }
  void AddNull(const Key& key) final {
    inserter_.AddArg(storage_.InternString(base::StringView(key.flat_key)),
                     storage_.InternString(base::StringView(key.key)),
                     Variadic::Null());
  }

  size_t GetArrayEntryIndex(const std::string& array_key) final {
    return inserter_.GetNextArrayEntryIndex(
        storage_.InternString(base::StringView(array_key)));
  }

  size_t IncrementArrayEntryIndex(const std::string& array_key) final {
    return inserter_.IncrementArrayEntryIndex(
        storage_.InternString(base::StringView(array_key)));
  }

  InternedMessageView* GetInternedMessageView(uint32_t field_id,
                                              uint64_t iid) final {
    return sequence_state_.GetInternedMessageView(field_id, iid);
  }

  int64_t packet_timestamp() final { return packet_timestamp_; }

  PacketSequenceStateGeneration* seq_state() final { return &sequence_state_; }

 private:
  int64_t packet_timestamp_;
  BoundInserter& inserter_;
  TraceStorage& storage_;
  PacketSequenceStateGeneration& sequence_state_;
};

TrackEventArgsParser::~TrackEventArgsParser() = default;

// Paths on Windows use backslash rather than slash as a separator.
// Normalise the paths by replacing backslashes with slashes to make it
// easier to write cross-platform scripts.
std::string NormalizePathSeparators(const protozero::ConstChars& path) {
  std::string result(path.data, path.size);
  for (char& c : result) {
    if (c == '\\')
      c = '/';
  }
  return result;
}

base::Optional<base::Status> MaybeParseUnsymbolizedSourceLocation(
    std::string prefix,
    const protozero::Field& field,
    util::ProtoToArgsParser::Delegate& delegate) {
  auto* decoder = delegate.GetInternedMessage(
      protos::pbzero::InternedData::kUnsymbolizedSourceLocations,
      field.as_uint64());
  if (!decoder) {
    // Lookup failed fall back on default behaviour which will just put
    // the iid into the args table.
    return base::nullopt;
  }
  // Interned mapping_id loses it's meaning when the sequence ends. So we need
  // to get an id from stack_profile_mapping table.
  ProfilePacketInternLookup intern_lookup(delegate.seq_state());
  auto mapping_id =
      delegate.seq_state()
          ->state()
          ->sequence_stack_profile_tracker()
          .FindOrInsertMapping(decoder->mapping_id(), &intern_lookup);
  if (!mapping_id) {
    return base::nullopt;
  }
  delegate.AddUnsignedInteger(
      util::ProtoToArgsParser::Key(prefix + ".mapping_id"), mapping_id->value);
  delegate.AddUnsignedInteger(util::ProtoToArgsParser::Key(prefix + ".rel_pc"),
                              decoder->rel_pc());
  return base::OkStatus();
}

base::Optional<base::Status> MaybeParseSourceLocation(
    std::string prefix,
    const protozero::Field& field,
    util::ProtoToArgsParser::Delegate& delegate) {
  auto* decoder = delegate.GetInternedMessage(
      protos::pbzero::InternedData::kSourceLocations, field.as_uint64());
  if (!decoder) {
    // Lookup failed fall back on default behaviour which will just put
    // the source_location_iid into the args table.
    return base::nullopt;
  }

  delegate.AddString(util::ProtoToArgsParser::Key(prefix + ".file_name"),
                     NormalizePathSeparators(decoder->file_name()));
  delegate.AddString(util::ProtoToArgsParser::Key(prefix + ".function_name"),
                     decoder->function_name());
  if (decoder->has_line_number()) {
    delegate.AddInteger(util::ProtoToArgsParser::Key(prefix + ".line_number"),
                        decoder->line_number());
  }

  return base::OkStatus();
}

}  // namespace

class TrackEventParser::EventImporter {
 public:
  EventImporter(TrackEventParser* parser,
                int64_t ts,
                const TrackEventData* event_data,
                ConstBytes blob,
                uint32_t packet_sequence_id)
      : context_(parser->context_),
        track_event_tracker_(parser->track_event_tracker_),
        storage_(context_->storage.get()),
        parser_(parser),
        args_translation_table_(context_->args_translation_table.get()),
        ts_(ts),
        event_data_(event_data),
        sequence_state_(event_data->trace_packet_data.sequence_state.get()),
        blob_(std::move(blob)),
        event_(blob_),
        legacy_event_(event_.legacy_event()),
        defaults_(event_data->trace_packet_data.sequence_state
                      ->GetTrackEventDefaults()),
        thread_timestamp_(event_data->thread_timestamp),
        thread_instruction_count_(event_data->thread_instruction_count),
        packet_sequence_id_(packet_sequence_id) {}

  util::Status Import() {
    // TODO(eseckler): This legacy event field will eventually be replaced by
    // fields in TrackEvent itself.
    if (PERFETTO_UNLIKELY(!event_.type() && !legacy_event_.has_phase()))
      return util::ErrStatus("TrackEvent without type or phase");

    category_id_ = ParseTrackEventCategory();
    name_id_ = ParseTrackEventName();

    if (context_->content_analyzer) {
      PacketAnalyzer::SampleAnnotation annotation;
      annotation.push_back({parser_->event_category_key_id_, category_id_});
      annotation.push_back({parser_->event_name_key_id_, name_id_});
      PacketAnalyzer::Get(context_)->ProcessPacket(
          event_data_->trace_packet_data.packet, annotation);
    }

    RETURN_IF_ERROR(ParseTrackAssociation());

    // Counter-type events don't support arguments (those are on the
    // CounterDescriptor instead). All they have is a |{double_,}counter_value|.
    if (event_.type() == TrackEvent::TYPE_COUNTER) {
      ParseCounterEvent();
      return util::OkStatus();
    }

    // If we have legacy thread time / instruction count fields, also parse them
    // into the counters tables.
    ParseLegacyThreadTimeAndInstructionsAsCounters();

    // Parse extra counter values before parsing the actual event. This way, we
    // can update the slice's thread time / instruction count fields based on
    // these counter values and also parse them as slice attributes / arguments.
    ParseExtraCounterValues();

    // TODO(eseckler): Replace phase with type and remove handling of
    // legacy_event_.phase() once it is no longer used by producers.
    char phase = static_cast<char>(ParsePhaseOrType());

    switch (phase) {
      case 'B':  // TRACE_EVENT_PHASE_BEGIN.
        return ParseThreadBeginEvent();
      case 'E':  // TRACE_EVENT_PHASE_END.
        return ParseThreadEndEvent();
      case 'X':  // TRACE_EVENT_PHASE_COMPLETE.
        return ParseThreadCompleteEvent();
      case 's':  // TRACE_EVENT_PHASE_FLOW_BEGIN.
      case 't':  // TRACE_EVENT_PHASE_FLOW_STEP.
      case 'f':  // TRACE_EVENT_PHASE_FLOW_END.
        return ParseFlowEventV1(phase);
      case 'i':
      case 'I':  // TRACE_EVENT_PHASE_INSTANT.
      case 'R':  // TRACE_EVENT_PHASE_MARK.
        return ParseThreadInstantEvent(phase);
      case 'b':  // TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN
      case 'S':
        return ParseAsyncBeginEvent(phase);
      case 'e':  // TRACE_EVENT_PHASE_NESTABLE_ASYNC_END
      case 'F':
        return ParseAsyncEndEvent();
      case 'n':  // TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT
        return ParseAsyncInstantEvent();
      case 'T':
      case 'p':
        return ParseAsyncStepEvent(phase);
      case 'M':  // TRACE_EVENT_PHASE_METADATA (process and thread names).
        return ParseMetadataEvent();
      default:
        // Other events are proxied via the raw table for JSON export.
        return ParseLegacyEventAsRawEvent();
    }
  }

 private:
  StringId ParseTrackEventCategory() {
    StringId category_id = kNullStringId;

    std::vector<uint64_t> category_iids;
    for (auto it = event_.category_iids(); it; ++it) {
      category_iids.push_back(*it);
    }
    std::vector<protozero::ConstChars> category_strings;
    for (auto it = event_.categories(); it; ++it) {
      category_strings.push_back(*it);
    }

    // If there's a single category, we can avoid building a concatenated
    // string.
    if (PERFETTO_LIKELY(category_iids.size() == 1 &&
                        category_strings.empty())) {
      auto* decoder = sequence_state_->LookupInternedMessage<
          protos::pbzero::InternedData::kEventCategoriesFieldNumber,
          protos::pbzero::EventCategory>(category_iids[0]);
      if (decoder) {
        category_id = storage_->InternString(decoder->name());
      } else {
        char buffer[32];
        base::StringWriter writer(buffer, sizeof(buffer));
        writer.AppendLiteral("unknown(");
        writer.AppendUnsignedInt(category_iids[0]);
        writer.AppendChar(')');
        category_id = storage_->InternString(writer.GetStringView());
      }
    } else if (category_iids.empty() && category_strings.size() == 1) {
      category_id = storage_->InternString(category_strings[0]);
    } else if (category_iids.size() + category_strings.size() > 1) {
      // We concatenate the category strings together since we currently only
      // support a single "cat" column.
      // TODO(eseckler): Support multi-category events in the table schema.
      std::string categories;
      for (uint64_t iid : category_iids) {
        auto* decoder = sequence_state_->LookupInternedMessage<
            protos::pbzero::InternedData::kEventCategoriesFieldNumber,
            protos::pbzero::EventCategory>(iid);
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
        category_id = storage_->InternString(base::StringView(categories));
    }

    return category_id;
  }

  StringId ParseTrackEventName() {
    uint64_t name_iid = event_.name_iid();
    if (!name_iid)
      name_iid = legacy_event_.name_iid();

    if (PERFETTO_LIKELY(name_iid)) {
      auto* decoder = sequence_state_->LookupInternedMessage<
          protos::pbzero::InternedData::kEventNamesFieldNumber,
          protos::pbzero::EventName>(name_iid);
      if (decoder)
        return storage_->InternString(decoder->name());
    } else if (event_.has_name()) {
      return storage_->InternString(event_.name());
    }

    return kNullStringId;
  }

  util::Status ParseTrackAssociation() {
    TrackTracker* track_tracker = context_->track_tracker.get();
    ProcessTracker* procs = context_->process_tracker.get();

    // Consider track_uuid from the packet and TrackEventDefaults, fall back to
    // the default descriptor track (uuid 0).
    track_uuid_ = event_.has_track_uuid()
                      ? event_.track_uuid()
                      : (defaults_ && defaults_->has_track_uuid()
                             ? defaults_->track_uuid()
                             : 0u);

    // Determine track from track_uuid specified in either TrackEvent or
    // TrackEventDefaults. If a non-default track is not set, we either:
    //   a) fall back to the track specified by the sequence's (or event's) pid
    //      + tid (only in case of legacy tracks/events, i.e. events that don't
    //      specify an explicit track uuid or use legacy event phases instead of
    //      TrackEvent types), or
    //   b) a default track.
    if (track_uuid_) {
      base::Optional<TrackId> opt_track_id =
          track_event_tracker_->GetDescriptorTrack(track_uuid_, name_id_,
                                                   packet_sequence_id_);
      if (!opt_track_id) {
        track_event_tracker_->ReserveDescriptorChildTrack(track_uuid_,
                                                          /*parent_uuid=*/0,
                                                          name_id_);
        opt_track_id = track_event_tracker_->GetDescriptorTrack(
            track_uuid_, name_id_, packet_sequence_id_);
      }
      track_id_ = *opt_track_id;

      auto thread_track_row =
          storage_->thread_track_table().id().IndexOf(track_id_);
      if (thread_track_row) {
        utid_ = storage_->thread_track_table().utid()[*thread_track_row];
        upid_ = storage_->thread_table().upid()[*utid_];
      } else {
        auto process_track_row =
            storage_->process_track_table().id().IndexOf(track_id_);
        if (process_track_row) {
          upid_ = storage_->process_track_table().upid()[*process_track_row];
          if (sequence_state_->state()->pid_and_tid_valid()) {
            uint32_t pid =
                static_cast<uint32_t>(sequence_state_->state()->pid());
            uint32_t tid =
                static_cast<uint32_t>(sequence_state_->state()->tid());
            UniqueTid utid_candidate = procs->UpdateThread(tid, pid);
            if (storage_->thread_table().upid()[utid_candidate] == upid_)
              legacy_passthrough_utid_ = utid_candidate;
          }
        } else {
          auto* tracks = context_->storage->mutable_track_table();
          auto track_index = tracks->id().IndexOf(track_id_);
          if (track_index) {
            const StringPool::Id& id = tracks->name()[*track_index];
            if (id.is_null())
              tracks->mutable_name()->Set(*track_index, name_id_);
          }

          if (sequence_state_->state()->pid_and_tid_valid()) {
            uint32_t pid =
                static_cast<uint32_t>(sequence_state_->state()->pid());
            uint32_t tid =
                static_cast<uint32_t>(sequence_state_->state()->tid());
            legacy_passthrough_utid_ = procs->UpdateThread(tid, pid);
          }
        }
      }
    } else {
      bool pid_tid_state_valid = sequence_state_->state()->pid_and_tid_valid();

      // We have a 0-value |track_uuid|. Nevertheless, we should only fall back
      // if we have either no |track_uuid| specified at all or |track_uuid| was
      // set explicitly to 0 (e.g. to override a default track_uuid) and we have
      // a legacy phase. Events with real phases should use |track_uuid| to
      // specify a different track (or use the pid/tid_override fields).
      bool fallback_to_legacy_pid_tid_tracks =
          (!event_.has_track_uuid() || !event_.has_type()) &&
          pid_tid_state_valid;

      // Always allow fallback if we have a process override.
      fallback_to_legacy_pid_tid_tracks |= legacy_event_.has_pid_override();

      // A thread override requires a valid pid.
      fallback_to_legacy_pid_tid_tracks |=
          legacy_event_.has_tid_override() && pid_tid_state_valid;

      if (fallback_to_legacy_pid_tid_tracks) {
        uint32_t pid = static_cast<uint32_t>(sequence_state_->state()->pid());
        uint32_t tid = static_cast<uint32_t>(sequence_state_->state()->tid());
        if (legacy_event_.has_pid_override()) {
          pid = static_cast<uint32_t>(legacy_event_.pid_override());
          tid = static_cast<uint32_t>(-1);
        }
        if (legacy_event_.has_tid_override())
          tid = static_cast<uint32_t>(legacy_event_.tid_override());

        utid_ = procs->UpdateThread(tid, pid);
        upid_ = storage_->thread_table().upid()[*utid_];
        track_id_ = track_tracker->InternThreadTrack(*utid_);
      } else {
        track_id_ = track_event_tracker_->GetOrCreateDefaultDescriptorTrack();
      }
    }

    if (!legacy_event_.has_phase())
      return util::OkStatus();

    // Legacy phases may imply a different track than the one specified by
    // the fallback (or default track uuid) above.
    switch (legacy_event_.phase()) {
      case 'b':
      case 'e':
      case 'n':
      case 'S':
      case 'T':
      case 'p':
      case 'F': {
        // Intern tracks for legacy async events based on legacy event ids.
        int64_t source_id = 0;
        bool source_id_is_process_scoped = false;
        if (legacy_event_.has_unscoped_id()) {
          source_id = static_cast<int64_t>(legacy_event_.unscoped_id());
        } else if (legacy_event_.has_global_id()) {
          source_id = static_cast<int64_t>(legacy_event_.global_id());
        } else if (legacy_event_.has_local_id()) {
          if (!upid_) {
            return util::ErrStatus(
                "TrackEvent with local_id without process association");
          }

          source_id = static_cast<int64_t>(legacy_event_.local_id());
          source_id_is_process_scoped = true;
        } else {
          return util::ErrStatus("Async LegacyEvent without ID");
        }

        // Catapult treats nestable async events of different categories with
        // the same ID as separate tracks. We replicate the same behavior
        // here. For legacy async events, it uses different tracks based on
        // event names.
        const bool legacy_async =
            legacy_event_.phase() == 'S' || legacy_event_.phase() == 'T' ||
            legacy_event_.phase() == 'p' || legacy_event_.phase() == 'F';
        StringId id_scope = legacy_async ? name_id_ : category_id_;
        if (legacy_event_.has_id_scope()) {
          std::string concat = storage_->GetString(category_id_).ToStdString() +
                               ":" + legacy_event_.id_scope().ToStdString();
          id_scope = storage_->InternString(base::StringView(concat));
        }

        track_id_ = context_->track_tracker->InternLegacyChromeAsyncTrack(
            name_id_, upid_.value_or(0), source_id, source_id_is_process_scoped,
            id_scope);
        legacy_passthrough_utid_ = utid_;
        break;
      }
      case 'i':
      case 'I': {
        // Intern tracks for global or process-scoped legacy instant events.
        switch (legacy_event_.instant_event_scope()) {
          case LegacyEvent::SCOPE_UNSPECIFIED:
          case LegacyEvent::SCOPE_THREAD:
            // Thread-scoped legacy instant events already have the right
            // track based on the tid/pid of the sequence.
            if (!utid_) {
              return util::ErrStatus(
                  "Thread-scoped instant event without thread association");
            }
            break;
          case LegacyEvent::SCOPE_GLOBAL:
            track_id_ = context_->track_tracker
                            ->GetOrCreateLegacyChromeGlobalInstantTrack();
            legacy_passthrough_utid_ = utid_;
            utid_ = base::nullopt;
            break;
          case LegacyEvent::SCOPE_PROCESS:
            if (!upid_) {
              return util::ErrStatus(
                  "Process-scoped instant event without process association");
            }

            track_id_ =
                context_->track_tracker->InternLegacyChromeProcessInstantTrack(
                    *upid_);
            legacy_passthrough_utid_ = utid_;
            utid_ = base::nullopt;
            break;
        }
        break;
      }
      default:
        break;
    }

    return util::OkStatus();
  }

  int32_t ParsePhaseOrType() {
    if (legacy_event_.has_phase())
      return legacy_event_.phase();

    switch (event_.type()) {
      case TrackEvent::TYPE_SLICE_BEGIN:
        return utid_ ? 'B' : 'b';
      case TrackEvent::TYPE_SLICE_END:
        return utid_ ? 'E' : 'e';
      case TrackEvent::TYPE_INSTANT:
        return utid_ ? 'i' : 'n';
      default:
        PERFETTO_ELOG("unexpected event type %d", event_.type());
        return 0;
    }
  }

  void ParseCounterEvent() {
    // Tokenizer ensures that TYPE_COUNTER events are associated with counter
    // tracks and have values.
    PERFETTO_DCHECK(storage_->counter_track_table().id().IndexOf(track_id_));
    PERFETTO_DCHECK(event_.has_counter_value() ||
                    event_.has_double_counter_value());

    context_->event_tracker->PushCounter(
        ts_, static_cast<double>(event_data_->counter_value), track_id_);
  }

  void ParseLegacyThreadTimeAndInstructionsAsCounters() {
    if (!utid_)
      return;
    // When these fields are set, we don't expect TrackDescriptor-based counters
    // for thread time or instruction count for this thread in the trace, so we
    // intern separate counter tracks based on name + utid. Note that we cannot
    // import the counter values from the end of a complete event, because the
    // EventTracker expects counters to be pushed in order of their timestamps.
    // One more reason to switch to split begin/end events.
    if (thread_timestamp_) {
      TrackId track_id = context_->track_tracker->InternThreadCounterTrack(
          parser_->counter_name_thread_time_id_, *utid_);
      context_->event_tracker->PushCounter(
          ts_, static_cast<double>(*thread_timestamp_), track_id);
    }
    if (thread_instruction_count_) {
      TrackId track_id = context_->track_tracker->InternThreadCounterTrack(
          parser_->counter_name_thread_instruction_count_id_, *utid_);
      context_->event_tracker->PushCounter(
          ts_, static_cast<double>(*thread_instruction_count_), track_id);
    }
  }

  void ParseExtraCounterValues() {
    if (!event_.has_extra_counter_values() &&
        !event_.has_extra_double_counter_values()) {
      return;
    }

    // Add integer extra counter values.
    size_t index = 0;
    protozero::RepeatedFieldIterator<uint64_t> track_uuid_it;
    if (event_.has_extra_counter_track_uuids()) {
      track_uuid_it = event_.extra_counter_track_uuids();
    } else if (defaults_ && defaults_->has_extra_counter_track_uuids()) {
      track_uuid_it = defaults_->extra_counter_track_uuids();
    }
    for (auto value_it = event_.extra_counter_values(); value_it;
         ++value_it, ++track_uuid_it, ++index) {
      AddExtraCounterValue(track_uuid_it, index);
    }

    // Add double extra counter values.
    track_uuid_it = protozero::RepeatedFieldIterator<uint64_t>();
    if (event_.has_extra_double_counter_track_uuids()) {
      track_uuid_it = event_.extra_double_counter_track_uuids();
    } else if (defaults_ && defaults_->has_extra_double_counter_track_uuids()) {
      track_uuid_it = defaults_->extra_double_counter_track_uuids();
    }
    for (auto value_it = event_.extra_double_counter_values(); value_it;
         ++value_it, ++track_uuid_it, ++index) {
      AddExtraCounterValue(track_uuid_it, index);
    }
  }

  void AddExtraCounterValue(
      protozero::RepeatedFieldIterator<uint64_t> track_uuid_it,
      size_t index) {
    // Tokenizer ensures that there aren't more values than uuids, that we
    // don't have more values than kMaxNumExtraCounters and that the
    // track_uuids are for valid counter tracks.
    PERFETTO_DCHECK(track_uuid_it);
    PERFETTO_DCHECK(index < TrackEventData::kMaxNumExtraCounters);

    base::Optional<TrackId> track_id = track_event_tracker_->GetDescriptorTrack(
        *track_uuid_it, kNullStringId, packet_sequence_id_);
    base::Optional<uint32_t> counter_row =
        storage_->counter_track_table().id().IndexOf(*track_id);

    double value = event_data_->extra_counter_values[index];
    context_->event_tracker->PushCounter(ts_, value, *track_id);

    // Also import thread_time and thread_instruction_count counters into
    // slice columns to simplify JSON export.
    StringId counter_name =
        storage_->counter_track_table().name()[*counter_row];
    if (counter_name == parser_->counter_name_thread_time_id_) {
      thread_timestamp_ = static_cast<int64_t>(value);
    } else if (counter_name ==
               parser_->counter_name_thread_instruction_count_id_) {
      thread_instruction_count_ = static_cast<int64_t>(value);
    }
  }

  util::Status ParseThreadBeginEvent() {
    if (!utid_) {
      return util::ErrStatus(
          "TrackEvent with phase B without thread association");
    }

    auto* thread_slices = storage_->mutable_slice_table();
    auto opt_slice_id = context_->slice_tracker->BeginTyped(
        thread_slices, MakeThreadSliceRow(),
        [this](BoundInserter* inserter) { ParseTrackEventArgs(inserter); });

    if (opt_slice_id.has_value()) {
      MaybeParseFlowEvents(opt_slice_id.value());
    }
    return util::OkStatus();
  }

  util::Status ParseThreadEndEvent() {
    if (!utid_) {
      return util::ErrStatus(
          "TrackEvent with phase E without thread association");
    }
    auto opt_slice_id = context_->slice_tracker->End(
        ts_, track_id_, category_id_, name_id_,
        [this](BoundInserter* inserter) { ParseTrackEventArgs(inserter); });
    if (!opt_slice_id)
      return base::OkStatus();

    MaybeParseFlowEvents(*opt_slice_id);
    auto* thread_slices = storage_->mutable_slice_table();
    auto opt_thread_slice_ref = thread_slices->FindById(*opt_slice_id);
    if (!opt_thread_slice_ref) {
      // This means that the end event did not match a corresponding track event
      // begin packet so we likely closed the wrong slice. There's not much we
      // can do about this beyond flag it as a stat.
      context_->storage->IncrementStats(stats::track_event_thread_invalid_end);
      return base::OkStatus();
    }

    tables::SliceTable::RowReference slice_ref = *opt_thread_slice_ref;
    base::Optional<int64_t> tts = slice_ref.thread_ts();
    if (tts) {
      PERFETTO_DCHECK(thread_timestamp_);
      slice_ref.set_thread_dur(*thread_timestamp_ - *tts);
    }
    base::Optional<int64_t> tic = slice_ref.thread_instruction_count();
    if (tic) {
      PERFETTO_DCHECK(event_data_->thread_instruction_count);
      slice_ref.set_thread_instruction_delta(
          *event_data_->thread_instruction_count - *tic);
    }
    return util::OkStatus();
  }

  util::Status ParseThreadCompleteEvent() {
    if (!utid_) {
      return util::ErrStatus(
          "TrackEvent with phase X without thread association");
    }

    auto duration_ns = legacy_event_.duration_us() * 1000;
    if (duration_ns < 0)
      return util::ErrStatus("TrackEvent with phase X with negative duration");

    auto* thread_slices = storage_->mutable_slice_table();
    tables::SliceTable::Row row = MakeThreadSliceRow();
    row.dur = duration_ns;
    if (legacy_event_.has_thread_duration_us()) {
      row.thread_dur = legacy_event_.thread_duration_us() * 1000;
    }
    if (legacy_event_.has_thread_instruction_delta()) {
      row.thread_instruction_delta = legacy_event_.thread_instruction_delta();
    }
    auto opt_slice_id = context_->slice_tracker->ScopedTyped(
        thread_slices, std::move(row),
        [this](BoundInserter* inserter) { ParseTrackEventArgs(inserter); });

    if (opt_slice_id.has_value()) {
      MaybeParseFlowEvents(opt_slice_id.value());
    }
    return util::OkStatus();
  }

  base::Optional<uint64_t> GetLegacyEventId() {
    if (legacy_event_.has_unscoped_id())
      return legacy_event_.unscoped_id();
    // TODO(andrewbb): Catapult doesn't support global_id and local_id on flow
    // events. We could add support in trace processor (e.g. because there seem
    // to be some callsites supplying local_id in chromium), but we would have
    // to consider the process ID for local IDs and use a separate ID scope for
    // global_id and unscoped_id.
    return base::nullopt;
  }

  util::Status ParseFlowEventV1(char phase) {
    auto opt_source_id = GetLegacyEventId();
    if (!opt_source_id) {
      storage_->IncrementStats(stats::flow_invalid_id);
      return util::ErrStatus("Invalid id for flow event v1");
    }
    FlowId flow_id = context_->flow_tracker->GetFlowIdForV1Event(
        opt_source_id.value(), category_id_, name_id_);
    switch (phase) {
      case 's':
        context_->flow_tracker->Begin(track_id_, flow_id);
        break;
      case 't':
        context_->flow_tracker->Step(track_id_, flow_id);
        break;
      case 'f':
        context_->flow_tracker->End(track_id_, flow_id,
                                    legacy_event_.bind_to_enclosing(),
                                    /* close_flow = */ false);
        break;
    }
    return util::OkStatus();
  }

  void MaybeParseTrackEventFlows(SliceId slice_id) {
    if (event_.has_flow_ids_old() || event_.has_flow_ids()) {
      auto it =
          event_.has_flow_ids() ? event_.flow_ids() : event_.flow_ids_old();
      for (; it; ++it) {
        FlowId flow_id = *it;
        if (!context_->flow_tracker->IsActive(flow_id)) {
          context_->flow_tracker->Begin(slice_id, flow_id);
          continue;
        }
        context_->flow_tracker->Step(slice_id, flow_id);
      }
    }
    if (event_.has_terminating_flow_ids_old() ||
        event_.has_terminating_flow_ids()) {
      auto it = event_.has_terminating_flow_ids()
                    ? event_.terminating_flow_ids()
                    : event_.terminating_flow_ids_old();
      for (; it; ++it) {
        FlowId flow_id = *it;
        if (!context_->flow_tracker->IsActive(flow_id)) {
          // If we should terminate a flow, do not begin a new one if it's not
          // active already.
          continue;
        }
        context_->flow_tracker->End(slice_id, flow_id,
                                    /* close_flow = */ true);
      }
    }
  }

  void MaybeParseFlowEventV2(SliceId slice_id) {
    if (!legacy_event_.has_bind_id()) {
      return;
    }
    if (!legacy_event_.has_flow_direction()) {
      storage_->IncrementStats(stats::flow_without_direction);
      return;
    }

    auto bind_id = legacy_event_.bind_id();
    switch (legacy_event_.flow_direction()) {
      case LegacyEvent::FLOW_OUT:
        context_->flow_tracker->Begin(slice_id, bind_id);
        break;
      case LegacyEvent::FLOW_INOUT:
        context_->flow_tracker->Step(slice_id, bind_id);
        break;
      case LegacyEvent::FLOW_IN:
        context_->flow_tracker->End(slice_id, bind_id,
                                    /* close_flow = */ false);
        break;
      default:
        storage_->IncrementStats(stats::flow_without_direction);
    }
  }

  void MaybeParseFlowEvents(SliceId slice_id) {
    MaybeParseFlowEventV2(slice_id);
    MaybeParseTrackEventFlows(slice_id);
  }

  util::Status ParseThreadInstantEvent(char phase) {
    // Handle instant events as slices with zero duration, so that they end
    // up nested underneath their parent slices.
    int64_t duration_ns = 0;
    int64_t tidelta = 0;
    base::Optional<tables::SliceTable::Id> opt_slice_id;
    auto args_inserter = [this, phase](BoundInserter* inserter) {
      ParseTrackEventArgs(inserter);
      // For legacy MARK event, add phase for JSON exporter.
      if (phase == 'R') {
        std::string phase_string(1, static_cast<char>(phase));
        StringId phase_id = storage_->InternString(phase_string.c_str());
        inserter->AddArg(parser_->legacy_event_phase_key_id_,
                         Variadic::String(phase_id));
      }
    };
    if (utid_) {
      auto* thread_slices = storage_->mutable_slice_table();
      tables::SliceTable::Row row = MakeThreadSliceRow();
      row.dur = duration_ns;
      if (thread_timestamp_) {
        row.thread_dur = duration_ns;
      }
      if (thread_instruction_count_) {
        row.thread_instruction_delta = tidelta;
      }
      opt_slice_id = context_->slice_tracker->ScopedTyped(
          thread_slices, row, std::move(args_inserter));
    } else {
      opt_slice_id = context_->slice_tracker->Scoped(
          ts_, track_id_, category_id_, name_id_, duration_ns,
          std::move(args_inserter));
    }
    if (!opt_slice_id.has_value()) {
      return util::OkStatus();
    }
    MaybeParseFlowEvents(opt_slice_id.value());
    return util::OkStatus();
  }

  util::Status ParseAsyncBeginEvent(char phase) {
    auto args_inserter = [this, phase](BoundInserter* inserter) {
      ParseTrackEventArgs(inserter);

      if (phase == 'b')
        return;
      PERFETTO_DCHECK(phase == 'S');
      // For legacy ASYNC_BEGIN, add phase for JSON exporter.
      std::string phase_string(1, static_cast<char>(phase));
      StringId phase_id = storage_->InternString(phase_string.c_str());
      inserter->AddArg(parser_->legacy_event_phase_key_id_,
                       Variadic::String(phase_id));
    };
    auto opt_slice_id = context_->slice_tracker->Begin(
        ts_, track_id_, category_id_, name_id_, args_inserter);
    if (!opt_slice_id.has_value()) {
      return util::OkStatus();
    }
    MaybeParseFlowEvents(opt_slice_id.value());
    // For the time being, we only create vtrack slice rows if we need to
    // store thread timestamps/counters.
    if (legacy_event_.use_async_tts()) {
      auto* vtrack_slices = storage_->mutable_virtual_track_slices();
      PERFETTO_DCHECK(!vtrack_slices->slice_count() ||
                      vtrack_slices->slice_ids().back() < opt_slice_id.value());
      int64_t tts = thread_timestamp_.value_or(0);
      int64_t tic = thread_instruction_count_.value_or(0);
      vtrack_slices->AddVirtualTrackSlice(opt_slice_id.value(), tts,
                                          kPendingThreadDuration, tic,
                                          kPendingThreadInstructionDelta);
    }
    return util::OkStatus();
  }

  util::Status ParseAsyncEndEvent() {
    auto opt_slice_id = context_->slice_tracker->End(
        ts_, track_id_, category_id_, name_id_,
        [this](BoundInserter* inserter) { ParseTrackEventArgs(inserter); });
    if (!opt_slice_id)
      return base::OkStatus();

    MaybeParseFlowEvents(*opt_slice_id);
    if (legacy_event_.use_async_tts()) {
      auto* vtrack_slices = storage_->mutable_virtual_track_slices();
      int64_t tts = event_data_->thread_timestamp.value_or(0);
      int64_t tic = event_data_->thread_instruction_count.value_or(0);
      vtrack_slices->UpdateThreadDeltasForSliceId(*opt_slice_id, tts, tic);
    }
    return util::OkStatus();
  }

  util::Status ParseAsyncStepEvent(char phase) {
    // Parse step events as instant events. Reconstructing the begin/end times
    // of the child slice would be too complicated, see b/178540838. For JSON
    // export, we still record the original step's phase in an arg.
    int64_t duration_ns = 0;
    context_->slice_tracker->Scoped(
        ts_, track_id_, category_id_, name_id_, duration_ns,
        [this, phase](BoundInserter* inserter) {
          ParseTrackEventArgs(inserter);

          PERFETTO_DCHECK(phase == 'T' || phase == 'p');
          std::string phase_string(1, static_cast<char>(phase));
          StringId phase_id = storage_->InternString(phase_string.c_str());
          inserter->AddArg(parser_->legacy_event_phase_key_id_,
                           Variadic::String(phase_id));
        });
    // Step events don't support thread timestamps, so no need to add a row to
    // virtual_track_slices.
    return util::OkStatus();
  }

  util::Status ParseAsyncInstantEvent() {
    // Handle instant events as slices with zero duration, so that they end
    // up nested underneath their parent slices.
    int64_t duration_ns = 0;
    int64_t tidelta = 0;
    auto opt_slice_id = context_->slice_tracker->Scoped(
        ts_, track_id_, category_id_, name_id_, duration_ns,
        [this](BoundInserter* inserter) { ParseTrackEventArgs(inserter); });
    if (!opt_slice_id.has_value()) {
      return util::OkStatus();
    }
    MaybeParseFlowEvents(opt_slice_id.value());
    if (legacy_event_.use_async_tts()) {
      auto* vtrack_slices = storage_->mutable_virtual_track_slices();
      PERFETTO_DCHECK(!vtrack_slices->slice_count() ||
                      vtrack_slices->slice_ids().back() < opt_slice_id.value());
      int64_t tts = thread_timestamp_.value_or(0);
      int64_t tic = thread_instruction_count_.value_or(0);
      vtrack_slices->AddVirtualTrackSlice(opt_slice_id.value(), tts,
                                          duration_ns, tic, tidelta);
    }
    return util::OkStatus();
  }

  util::Status ParseMetadataEvent() {
    ProcessTracker* procs = context_->process_tracker.get();

    if (name_id_ == kNullStringId)
      return util::ErrStatus("Metadata event without name");

    // Parse process and thread names from correspondingly named events.
    NullTermStringView event_name = storage_->GetString(name_id_);
    PERFETTO_DCHECK(event_name.data());
    if (strcmp(event_name.c_str(), "thread_name") == 0) {
      if (!utid_) {
        return util::ErrStatus(
            "thread_name metadata event without thread association");
      }

      auto it = event_.debug_annotations();
      if (!it) {
        return util::ErrStatus(
            "thread_name metadata event without debug annotations");
      }
      protos::pbzero::DebugAnnotation::Decoder annotation(*it);
      auto thread_name = annotation.string_value();
      if (!thread_name.size)
        return util::OkStatus();
      auto thread_name_id = storage_->InternString(thread_name);
      procs->UpdateThreadNameByUtid(
          *utid_, thread_name_id,
          ThreadNamePriority::kTrackDescriptorThreadType);
      return util::OkStatus();
    }
    if (strcmp(event_name.c_str(), "process_name") == 0) {
      if (!upid_) {
        return util::ErrStatus(
            "process_name metadata event without process association");
      }

      auto it = event_.debug_annotations();
      if (!it) {
        return util::ErrStatus(
            "process_name metadata event without debug annotations");
      }
      protos::pbzero::DebugAnnotation::Decoder annotation(*it);
      auto process_name = annotation.string_value();
      if (!process_name.size)
        return util::OkStatus();
      auto process_name_id =
          storage_->InternString(base::StringView(process_name));
      // Don't override system-provided names.
      procs->SetProcessNameIfUnset(*upid_, process_name_id);
      return util::OkStatus();
    }
    // Other metadata events are proxied via the raw table for JSON export.
    ParseLegacyEventAsRawEvent();
    return util::OkStatus();
  }

  util::Status ParseLegacyEventAsRawEvent() {
    if (!utid_)
      return util::ErrStatus("raw legacy event without thread association");

    RawId id = storage_->mutable_raw_table()
                   ->Insert({ts_, parser_->raw_legacy_event_id_, 0, *utid_})
                   .id;

    auto inserter = context_->args_tracker->AddArgsTo(id);
    inserter
        .AddArg(parser_->legacy_event_category_key_id_,
                Variadic::String(category_id_))
        .AddArg(parser_->legacy_event_name_key_id_, Variadic::String(name_id_));

    std::string phase_string(1, static_cast<char>(legacy_event_.phase()));
    StringId phase_id = storage_->InternString(phase_string.c_str());
    inserter.AddArg(parser_->legacy_event_phase_key_id_,
                    Variadic::String(phase_id));

    if (legacy_event_.has_duration_us()) {
      inserter.AddArg(parser_->legacy_event_duration_ns_key_id_,
                      Variadic::Integer(legacy_event_.duration_us() * 1000));
    }

    if (thread_timestamp_) {
      inserter.AddArg(parser_->legacy_event_thread_timestamp_ns_key_id_,
                      Variadic::Integer(*thread_timestamp_));
      if (legacy_event_.has_thread_duration_us()) {
        inserter.AddArg(
            parser_->legacy_event_thread_duration_ns_key_id_,
            Variadic::Integer(legacy_event_.thread_duration_us() * 1000));
      }
    }

    if (thread_instruction_count_) {
      inserter.AddArg(parser_->legacy_event_thread_instruction_count_key_id_,
                      Variadic::Integer(*thread_instruction_count_));
      if (legacy_event_.has_thread_instruction_delta()) {
        inserter.AddArg(
            parser_->legacy_event_thread_instruction_delta_key_id_,
            Variadic::Integer(legacy_event_.thread_instruction_delta()));
      }
    }

    if (legacy_event_.use_async_tts()) {
      inserter.AddArg(parser_->legacy_event_use_async_tts_key_id_,
                      Variadic::Boolean(true));
    }

    bool has_id = false;
    if (legacy_event_.has_unscoped_id()) {
      // Unscoped ids are either global or local depending on the phase. Pass
      // them through as unscoped IDs to JSON export to preserve this behavior.
      inserter.AddArg(parser_->legacy_event_unscoped_id_key_id_,
                      Variadic::UnsignedInteger(legacy_event_.unscoped_id()));
      has_id = true;
    } else if (legacy_event_.has_global_id()) {
      inserter.AddArg(parser_->legacy_event_global_id_key_id_,
                      Variadic::UnsignedInteger(legacy_event_.global_id()));
      has_id = true;
    } else if (legacy_event_.has_local_id()) {
      inserter.AddArg(parser_->legacy_event_local_id_key_id_,
                      Variadic::UnsignedInteger(legacy_event_.local_id()));
      has_id = true;
    }

    if (has_id && legacy_event_.has_id_scope() &&
        legacy_event_.id_scope().size) {
      inserter.AddArg(
          parser_->legacy_event_id_scope_key_id_,
          Variadic::String(storage_->InternString(legacy_event_.id_scope())));
    }

    // No need to parse legacy_event.instant_event_scope() because we import
    // instant events into the slice table.

    ParseTrackEventArgs(&inserter);
    return util::OkStatus();
  }

  void ParseTrackEventArgs(BoundInserter* inserter) {
    auto log_errors = [this](util::Status status) {
      if (status.ok())
        return;
      // Log error but continue parsing the other args.
      storage_->IncrementStats(stats::track_event_parser_errors);
      PERFETTO_DLOG("ParseTrackEventArgs error: %s", status.c_message());
    };

    if (event_.has_source_location_iid()) {
      log_errors(AddSourceLocationArgs(event_.source_location_iid(), inserter));
    }

    if (event_.has_task_execution()) {
      log_errors(ParseTaskExecutionArgs(event_.task_execution(), inserter));
    }
    if (event_.has_log_message()) {
      log_errors(ParseLogMessage(event_.log_message(), inserter));
    }
    if (event_.has_chrome_histogram_sample()) {
      log_errors(
          ParseHistogramName(event_.chrome_histogram_sample(), inserter));
    }
    if (event_.has_chrome_active_processes()) {
      protos::pbzero::ChromeActiveProcesses::Decoder message(
          event_.chrome_active_processes());
      for (auto it = message.pid(); it; ++it) {
        parser_->AddActiveProcess(ts_, *it);
      }
    }

    TrackEventArgsParser args_writer(ts_, *inserter, *storage_,
                                     *sequence_state_);
    int unknown_extensions = 0;
    log_errors(parser_->args_parser_.ParseMessage(
        blob_, ".perfetto.protos.TrackEvent", &parser_->reflect_fields_,
        args_writer, &unknown_extensions));
    if (unknown_extensions > 0) {
      context_->storage->IncrementStats(stats::unknown_extension_fields,
                                        unknown_extensions);
    }

    {
      auto key = parser_->args_parser_.EnterDictionary("debug");
      util::DebugAnnotationParser parser(parser_->args_parser_);
      for (auto it = event_.debug_annotations(); it; ++it) {
        log_errors(parser.Parse(*it, args_writer));
      }
    }

    if (legacy_passthrough_utid_) {
      inserter->AddArg(parser_->legacy_event_passthrough_utid_id_,
                       Variadic::UnsignedInteger(*legacy_passthrough_utid_),
                       ArgsTracker::UpdatePolicy::kSkipIfExists);
    }
  }

  util::Status ParseTaskExecutionArgs(ConstBytes task_execution,
                                      BoundInserter* inserter) {
    protos::pbzero::TaskExecution::Decoder task(task_execution);
    uint64_t iid = task.posted_from_iid();
    if (!iid)
      return util::ErrStatus("TaskExecution with invalid posted_from_iid");

    auto* decoder = sequence_state_->LookupInternedMessage<
        protos::pbzero::InternedData::kSourceLocationsFieldNumber,
        protos::pbzero::SourceLocation>(iid);
    if (!decoder)
      return util::ErrStatus("TaskExecution with invalid posted_from_iid");

    StringId file_name_id = kNullStringId;
    StringId function_name_id = kNullStringId;
    uint32_t line_number = 0;

    std::string file_name = NormalizePathSeparators(decoder->file_name());
    file_name_id = storage_->InternString(base::StringView(file_name));
    function_name_id = storage_->InternString(decoder->function_name());
    line_number = decoder->line_number();

    inserter->AddArg(parser_->task_file_name_args_key_id_,
                     Variadic::String(file_name_id));
    inserter->AddArg(parser_->task_function_name_args_key_id_,
                     Variadic::String(function_name_id));
    inserter->AddArg(parser_->task_line_number_args_key_id_,
                     Variadic::UnsignedInteger(line_number));
    return util::OkStatus();
  }

  util::Status AddSourceLocationArgs(uint64_t iid, BoundInserter* inserter) {
    if (!iid)
      return util::ErrStatus("SourceLocation with invalid iid");

    auto* decoder = sequence_state_->LookupInternedMessage<
        protos::pbzero::InternedData::kSourceLocationsFieldNumber,
        protos::pbzero::SourceLocation>(iid);
    if (!decoder)
      return util::ErrStatus("SourceLocation with invalid iid");

    StringId file_name_id = kNullStringId;
    StringId function_name_id = kNullStringId;
    uint32_t line_number = 0;

    std::string file_name = NormalizePathSeparators(decoder->file_name());
    file_name_id = storage_->InternString(base::StringView(file_name));
    function_name_id = storage_->InternString(decoder->function_name());
    line_number = decoder->line_number();

    inserter->AddArg(parser_->source_location_file_name_key_id_,
                     Variadic::String(file_name_id));
    inserter->AddArg(parser_->source_location_function_name_key_id_,
                     Variadic::String(function_name_id));
    inserter->AddArg(parser_->source_location_line_number_key_id_,
                     Variadic::UnsignedInteger(line_number));
    return util::OkStatus();
  }

  util::Status ParseLogMessage(ConstBytes blob, BoundInserter* inserter) {
    if (!utid_)
      return util::ErrStatus("LogMessage without thread association");

    protos::pbzero::LogMessage::Decoder message(blob);

    auto* body_decoder = sequence_state_->LookupInternedMessage<
        protos::pbzero::InternedData::kLogMessageBodyFieldNumber,
        protos::pbzero::LogMessageBody>(message.body_iid());
    if (!body_decoder)
      return util::ErrStatus("LogMessage with invalid body_iid");

    const StringId log_message_id =
        storage_->InternString(body_decoder->body());
    inserter->AddArg(parser_->log_message_body_key_id_,
                     Variadic::String(log_message_id));

    StringId source_location_id = kNullStringId;
    if (message.has_source_location_iid()) {
      auto* source_location_decoder = sequence_state_->LookupInternedMessage<
          protos::pbzero::InternedData::kSourceLocationsFieldNumber,
          protos::pbzero::SourceLocation>(message.source_location_iid());
      if (!source_location_decoder)
        return util::ErrStatus("LogMessage with invalid source_location_iid");
      const std::string source_location =
          source_location_decoder->file_name().ToStdString() + ":" +
          std::to_string(source_location_decoder->line_number());
      source_location_id =
          storage_->InternString(base::StringView(source_location));

      inserter->AddArg(parser_->log_message_source_location_file_name_key_id_,
                       Variadic::String(storage_->InternString(
                           source_location_decoder->file_name())));
      inserter->AddArg(
          parser_->log_message_source_location_function_name_key_id_,
          Variadic::String(storage_->InternString(
              source_location_decoder->function_name())));
      inserter->AddArg(
          parser_->log_message_source_location_line_number_key_id_,
          Variadic::Integer(source_location_decoder->line_number()));
    }

    storage_->mutable_android_log_table()->Insert(
        {ts_, *utid_,
         /*priority*/ 0,
         /*tag_id*/ source_location_id, log_message_id});

    return util::OkStatus();
  }

  util::Status ParseHistogramName(ConstBytes blob, BoundInserter* inserter) {
    protos::pbzero::ChromeHistogramSample::Decoder sample(blob);
    if (!sample.has_name_iid())
      return util::OkStatus();

    if (sample.has_name()) {
      return util::ErrStatus(
          "name is already set for ChromeHistogramSample: only one of name and "
          "name_iid can be set.");
    }

    auto* decoder = sequence_state_->LookupInternedMessage<
        protos::pbzero::InternedData::kHistogramNamesFieldNumber,
        protos::pbzero::HistogramName>(sample.name_iid());
    if (!decoder)
      return util::ErrStatus("HistogramName with invalid name_iid");

    inserter->AddArg(parser_->histogram_name_key_id_,
                     Variadic::String(storage_->InternString(decoder->name())));
    return util::OkStatus();
  }

  tables::SliceTable::Row MakeThreadSliceRow() {
    tables::SliceTable::Row row;
    row.ts = ts_;
    row.track_id = track_id_;
    row.category = category_id_;
    row.name = name_id_;
    row.thread_ts = thread_timestamp_;
    row.thread_dur = base::nullopt;
    row.thread_instruction_count = thread_instruction_count_;
    row.thread_instruction_delta = base::nullopt;
    return row;
  }

  TraceProcessorContext* context_;
  TrackEventTracker* track_event_tracker_;
  TraceStorage* storage_;
  TrackEventParser* parser_;
  ArgsTranslationTable* args_translation_table_;
  int64_t ts_;
  const TrackEventData* event_data_;
  PacketSequenceStateGeneration* sequence_state_;
  ConstBytes blob_;
  TrackEvent::Decoder event_;
  LegacyEvent::Decoder legacy_event_;
  protos::pbzero::TrackEventDefaults::Decoder* defaults_;

  // Importing state.
  StringId category_id_;
  StringId name_id_;
  uint64_t track_uuid_;
  TrackId track_id_;
  base::Optional<UniqueTid> utid_;
  base::Optional<UniqueTid> upid_;
  base::Optional<int64_t> thread_timestamp_;
  base::Optional<int64_t> thread_instruction_count_;
  // All events in legacy JSON require a thread ID, but for some types of
  // events (e.g. async events or process/global-scoped instants), we don't
  // store it in the slice/track model. To pass the utid through to the json
  // export, we store it in an arg.
  base::Optional<UniqueTid> legacy_passthrough_utid_;

  uint32_t packet_sequence_id_;
};

TrackEventParser::TrackEventParser(TraceProcessorContext* context,
                                   TrackEventTracker* track_event_tracker)
    : args_parser_(*context->descriptor_pool_.get()),
      context_(context),
      track_event_tracker_(track_event_tracker),
      counter_name_thread_time_id_(
          context->storage->InternString("thread_time")),
      counter_name_thread_instruction_count_id_(
          context->storage->InternString("thread_instruction_count")),
      task_file_name_args_key_id_(
          context->storage->InternString("task.posted_from.file_name")),
      task_function_name_args_key_id_(
          context->storage->InternString("task.posted_from.function_name")),
      task_line_number_args_key_id_(
          context->storage->InternString("task.posted_from.line_number")),
      log_message_body_key_id_(
          context->storage->InternString("track_event.log_message")),
      log_message_source_location_function_name_key_id_(
          context->storage->InternString(
              "track_event.log_message.function_name")),
      log_message_source_location_file_name_key_id_(
          context->storage->InternString("track_event.log_message.file_name")),
      log_message_source_location_line_number_key_id_(
          context->storage->InternString(
              "track_event.log_message.line_number")),
      source_location_function_name_key_id_(
          context->storage->InternString("source.function_name")),
      source_location_file_name_key_id_(
          context->storage->InternString("source.file_name")),
      source_location_line_number_key_id_(
          context->storage->InternString("source.line_number")),
      raw_legacy_event_id_(
          context->storage->InternString("track_event.legacy_event")),
      legacy_event_passthrough_utid_id_(
          context->storage->InternString("legacy_event.passthrough_utid")),
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
      histogram_name_key_id_(
          context->storage->InternString("chrome_histogram_sample.name")),
      flow_direction_value_in_id_(context->storage->InternString("in")),
      flow_direction_value_out_id_(context->storage->InternString("out")),
      flow_direction_value_inout_id_(context->storage->InternString("inout")),
      chrome_legacy_ipc_class_args_key_id_(
          context->storage->InternString("legacy_ipc.class")),
      chrome_legacy_ipc_line_args_key_id_(
          context->storage->InternString("legacy_ipc.line")),
      chrome_host_app_package_name_id_(
          context->storage->InternString("chrome.host_app_package_name")),
      chrome_crash_trace_id_name_id_(
          context->storage->InternString("chrome.crash_trace_id")),
      chrome_process_label_flat_key_id_(
          context->storage->InternString("chrome.process_label")),
      chrome_process_type_id_(
          context_->storage->InternString("chrome.process_type")),
      event_category_key_id_(context_->storage->InternString("event.category")),
      event_name_key_id_(context_->storage->InternString("event.name")),
      chrome_string_lookup_(context->storage.get()),
      counter_unit_ids_{{kNullStringId, context_->storage->InternString("ns"),
                         context_->storage->InternString("count"),
                         context_->storage->InternString("bytes")}},
      active_chrome_processes_tracker_(context) {
  args_parser_.AddParsingOverrideForField(
      "chrome_mojo_event_info.mojo_interface_method_iid",
      [](const protozero::Field& field,
         util::ProtoToArgsParser::Delegate& delegate) {
        return MaybeParseUnsymbolizedSourceLocation(
            "chrome_mojo_event_info.mojo_interface_method.native_symbol", field,
            delegate);
      });
  // Switch |source_location_iid| into its interned data variant.
  args_parser_.AddParsingOverrideForField(
      "begin_impl_frame_args.current_args.source_location_iid",
      [](const protozero::Field& field,
         util::ProtoToArgsParser::Delegate& delegate) {
        return MaybeParseSourceLocation("begin_impl_frame_args.current_args",
                                        field, delegate);
      });
  args_parser_.AddParsingOverrideForField(
      "begin_impl_frame_args.last_args.source_location_iid",
      [](const protozero::Field& field,
         util::ProtoToArgsParser::Delegate& delegate) {
        return MaybeParseSourceLocation("begin_impl_frame_args.last_args",
                                        field, delegate);
      });
  args_parser_.AddParsingOverrideForField(
      "begin_frame_observer_state.last_begin_frame_args.source_location_iid",
      [](const protozero::Field& field,
         util::ProtoToArgsParser::Delegate& delegate) {
        return MaybeParseSourceLocation(
            "begin_frame_observer_state.last_begin_frame_args", field,
            delegate);
      });
  args_parser_.AddParsingOverrideForField(
      "chrome_memory_pressure_notification.creation_location_iid",
      [](const protozero::Field& field,
         util::ProtoToArgsParser::Delegate& delegate) {
        return MaybeParseSourceLocation("chrome_memory_pressure_notification",
                                        field, delegate);
      });

  // Parse DebugAnnotations.
  args_parser_.AddParsingOverrideForType(
      ".perfetto.protos.DebugAnnotation",
      [&](util::ProtoToArgsParser::ScopedNestedKeyContext& key,
          const protozero::ConstBytes& data,
          util::ProtoToArgsParser::Delegate& delegate) {
        // Do not add "debug_annotations" to the final key.
        key.RemoveFieldSuffix();
        util::DebugAnnotationParser annotation_parser(args_parser_);
        return annotation_parser.Parse(data, delegate);
      });

  args_parser_.AddParsingOverrideForField(
      "active_processes.pid", [&](const protozero::Field& field,
                                  util::ProtoToArgsParser::Delegate& delegate) {
        AddActiveProcess(delegate.packet_timestamp(), field.as_int32());
        // Fallthrough so that the parser adds pid as a regular arg.
        return base::nullopt;
      });

  for (uint16_t index : kReflectFields) {
    reflect_fields_.push_back(index);
  }
}

void TrackEventParser::ParseTrackDescriptor(
    int64_t packet_timestamp,
    protozero::ConstBytes track_descriptor,
    uint32_t packet_sequence_id) {
  protos::pbzero::TrackDescriptor::Decoder decoder(track_descriptor);

  // Ensure that the track and its parents are resolved. This may start a new
  // process and/or thread (i.e. new upid/utid).
  TrackId track_id = *track_event_tracker_->GetDescriptorTrack(
      decoder.uuid(), kNullStringId, packet_sequence_id);

  if (decoder.has_thread()) {
    UniqueTid utid = ParseThreadDescriptor(decoder.thread());
    if (decoder.has_chrome_thread())
      ParseChromeThreadDescriptor(utid, decoder.chrome_thread());
  } else if (decoder.has_process()) {
    UniquePid upid =
        ParseProcessDescriptor(packet_timestamp, decoder.process());
    if (decoder.has_chrome_process())
      ParseChromeProcessDescriptor(upid, decoder.chrome_process());
  } else if (decoder.has_counter()) {
    ParseCounterDescriptor(track_id, decoder.counter());
  }

  // Override the name with the most recent name seen (after sorting by ts).
  if (decoder.has_name()) {
    auto* tracks = context_->storage->mutable_track_table();
    StringId name_id = context_->storage->InternString(decoder.name());
    tracks->mutable_name()->Set(*tracks->id().IndexOf(track_id), name_id);
  }
}

UniquePid TrackEventParser::ParseProcessDescriptor(
    int64_t packet_timestamp,
    protozero::ConstBytes process_descriptor) {
  protos::pbzero::ProcessDescriptor::Decoder decoder(process_descriptor);
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(decoder.pid()));
  active_chrome_processes_tracker_.AddProcessDescriptor(packet_timestamp, upid);
  if (decoder.has_process_name() && decoder.process_name().size) {
    // Don't override system-provided names.
    context_->process_tracker->SetProcessNameIfUnset(
        upid, context_->storage->InternString(decoder.process_name()));
  }
  if (decoder.has_start_timestamp_ns() && decoder.start_timestamp_ns() > 0) {
    context_->process_tracker->SetStartTsIfUnset(upid,
                                                 decoder.start_timestamp_ns());
  }
  // TODO(skyostil): Remove parsing for legacy chrome_process_type field.
  if (decoder.has_chrome_process_type()) {
    StringId name_id =
        chrome_string_lookup_.GetProcessName(decoder.chrome_process_type());
    // Don't override system-provided names.
    context_->process_tracker->SetProcessNameIfUnset(upid, name_id);
  }
  int label_index = 0;
  for (auto it = decoder.process_labels(); it; it++) {
    StringId label_id = context_->storage->InternString(*it);
    std::string key = "chrome.process_label[";
    key.append(std::to_string(label_index++));
    key.append("]");
    context_->process_tracker->AddArgsTo(upid).AddArg(
        chrome_process_label_flat_key_id_,
        context_->storage->InternString(base::StringView(key)),
        Variadic::String(label_id));
  }
  return upid;
}

void TrackEventParser::ParseChromeProcessDescriptor(
    UniquePid upid,
    protozero::ConstBytes chrome_process_descriptor) {
  protos::pbzero::ChromeProcessDescriptor::Decoder decoder(
      chrome_process_descriptor);

  StringId name_id =
      chrome_string_lookup_.GetProcessName(decoder.process_type());
  // Don't override system-provided names.
  context_->process_tracker->SetProcessNameIfUnset(upid, name_id);

  ArgsTracker::BoundInserter process_args =
      context_->process_tracker->AddArgsTo(upid);
  // For identifying Chrome processes in system traces.
  process_args.AddArg(chrome_process_type_id_, Variadic::String(name_id));
  if (decoder.has_host_app_package_name()) {
    process_args.AddArg(chrome_host_app_package_name_id_,
                        Variadic::String(context_->storage->InternString(
                            decoder.host_app_package_name())));
  }
  if (decoder.has_crash_trace_id()) {
    process_args.AddArg(chrome_crash_trace_id_name_id_,
                        Variadic::UnsignedInteger(decoder.crash_trace_id()));
  }
}

UniqueTid TrackEventParser::ParseThreadDescriptor(
    protozero::ConstBytes thread_descriptor) {
  protos::pbzero::ThreadDescriptor::Decoder decoder(thread_descriptor);
  UniqueTid utid = context_->process_tracker->UpdateThread(
      static_cast<uint32_t>(decoder.tid()),
      static_cast<uint32_t>(decoder.pid()));
  StringId name_id = kNullStringId;
  if (decoder.has_thread_name() && decoder.thread_name().size) {
    name_id = context_->storage->InternString(decoder.thread_name());
  } else if (decoder.has_chrome_thread_type()) {
    // TODO(skyostil): Remove parsing for legacy chrome_thread_type field.
    name_id = chrome_string_lookup_.GetThreadName(decoder.chrome_thread_type());
  }
  context_->process_tracker->UpdateThreadNameByUtid(
      utid, name_id, ThreadNamePriority::kTrackDescriptor);
  return utid;
}

void TrackEventParser::ParseChromeThreadDescriptor(
    UniqueTid utid,
    protozero::ConstBytes chrome_thread_descriptor) {
  protos::pbzero::ChromeThreadDescriptor::Decoder decoder(
      chrome_thread_descriptor);
  if (!decoder.has_thread_type())
    return;

  StringId name_id = chrome_string_lookup_.GetThreadName(decoder.thread_type());
  context_->process_tracker->UpdateThreadNameByUtid(
      utid, name_id, ThreadNamePriority::kTrackDescriptorThreadType);
}

void TrackEventParser::ParseCounterDescriptor(
    TrackId track_id,
    protozero::ConstBytes counter_descriptor) {
  using protos::pbzero::CounterDescriptor;

  CounterDescriptor::Decoder decoder(counter_descriptor);
  auto* counter_tracks = context_->storage->mutable_counter_track_table();

  size_t unit_index = static_cast<size_t>(decoder.unit());
  if (unit_index >= counter_unit_ids_.size())
    unit_index = CounterDescriptor::UNIT_UNSPECIFIED;

  auto opt_track_idx = counter_tracks->id().IndexOf(track_id);
  if (!opt_track_idx) {
    context_->storage->IncrementStats(stats::track_event_parser_errors);
    return;
  }

  auto track_idx = *opt_track_idx;

  switch (decoder.type()) {
    case CounterDescriptor::COUNTER_UNSPECIFIED:
      break;
    case CounterDescriptor::COUNTER_THREAD_TIME_NS:
      unit_index = CounterDescriptor::UNIT_TIME_NS;
      counter_tracks->mutable_name()->Set(track_idx,
                                          counter_name_thread_time_id_);
      break;
    case CounterDescriptor::COUNTER_THREAD_INSTRUCTION_COUNT:
      unit_index = CounterDescriptor::UNIT_COUNT;
      counter_tracks->mutable_name()->Set(
          track_idx, counter_name_thread_instruction_count_id_);
      break;
  }

  counter_tracks->mutable_unit()->Set(track_idx, counter_unit_ids_[unit_index]);
}

void TrackEventParser::ParseTrackEvent(int64_t ts,
                                       const TrackEventData* event_data,
                                       ConstBytes blob,
                                       uint32_t packet_sequence_id) {
  const auto range_of_interest_start_us =
      track_event_tracker_->range_of_interest_start_us();
  if (context_->config.drop_track_event_data_before ==
          DropTrackEventDataBefore::kTrackEventRangeOfInterest &&
      range_of_interest_start_us && ts < *range_of_interest_start_us * 1000) {
    // The event is outside of the range of interest, and dropping is enabled.
    // So we drop the event.
    context_->storage->IncrementStats(
        stats::track_event_dropped_packets_outside_of_range_of_interest);
    return;
  }
  util::Status status =
      EventImporter(this, ts, event_data, std::move(blob), packet_sequence_id)
          .Import();
  if (!status.ok()) {
    context_->storage->IncrementStats(stats::track_event_parser_errors);
    PERFETTO_DLOG("ParseTrackEvent error: %s", status.c_message());
  }
}

void TrackEventParser::AddActiveProcess(int64_t packet_timestamp, int32_t pid) {
  UniquePid upid =
      context_->process_tracker->GetOrCreateProcess(static_cast<uint32_t>(pid));
  active_chrome_processes_tracker_.AddActiveProcessMetadata(packet_timestamp,
                                                            upid);
}

void TrackEventParser::NotifyEndOfFile() {
  active_chrome_processes_tracker_.NotifyEndOfFile();
}

}  // namespace trace_processor
}  // namespace perfetto
