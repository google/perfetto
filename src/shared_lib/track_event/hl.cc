/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/public/abi/track_event_hl_abi.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/public/track_event.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "src/shared_lib/track_event/ds.h"
#include "src/shared_lib/track_event/serialization.h"

namespace perfetto::shlib {
namespace {

using perfetto::internal::TrackEventInternal;
// All interned string messages for track events must have this field number
// structure.
static constexpr uint32_t kInternedStringIidFieldNumber = 1;
static constexpr uint32_t kInternedStringNameFieldNumber = 2;

protos::pbzero::TrackEvent::Type EventType(int32_t type) {
  using Type = protos::pbzero::TrackEvent::Type;
  auto enum_type = static_cast<PerfettoTeType>(type);
  switch (enum_type) {
    case PERFETTO_TE_TYPE_SLICE_BEGIN:
      return Type::TYPE_SLICE_BEGIN;
    case PERFETTO_TE_TYPE_SLICE_END:
      return Type::TYPE_SLICE_END;
    case PERFETTO_TE_TYPE_INSTANT:
      return Type::TYPE_INSTANT;
    case PERFETTO_TE_TYPE_COUNTER:
      return Type::TYPE_COUNTER;
    case PERFETTO_TE_TYPE_STATE:
      return Type::TYPE_STATE;
  }
  return Type::TYPE_UNSPECIFIED;
}

// Parts of an event that every instance writes in the same way.
// Pointers borrow the caller's storage for the duration of emit.
struct EventParams {
  // From the emit call.
  const PerfettoTeCategoryImpl* cat = nullptr;
  // Name of the event and its size in bytes, without a terminating NUL.
  const char* name = nullptr;
  size_t name_size = 0;
  const PerfettoTeHlExtra* const* extra_data = nullptr;

  // Derived from the extras to avoid recomputing for each instance.
  TraceTimestamp timestamp{};
  // The last track extra, or nullptr for the default track.
  const PerfettoTeHlExtra* track = nullptr;
  // Only set if `track` is a PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK.
  uint64_t named_track_uuid = 0;
  const PerfettoTeCategoryDescriptor* dynamic_cat = nullptr;

  // Small fields last to avoid padding.
  protos::pbzero::TrackEvent::Type type =
      protos::pbzero::TrackEvent::TYPE_UNSPECIFIED;
  bool intern_name = true;
  bool flush = false;
  // Lets WriteTrackEvent() skip its custom proto pass when no
  // PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS extra is present.
  bool has_proto_fields = false;
};

// Prepares EventParams and scans `extra_data` once for the parts of the event
// that every instance shares.
EventParams PrepareEvent(const PerfettoTeCategoryImpl* cat,
                         int32_t type,
                         const char* name,
                         size_t name_size,
                         const PerfettoTeHlExtra* const* extra_data) {
  EventParams params;
  params.cat = cat;
  params.type = EventType(type);
  params.name = name;
  params.name_size = name_size;
  params.extra_data = extra_data;

  const PerfettoTeHlExtraTimestamp* custom_timestamp = nullptr;
  for (const auto* it = extra_data; *it != nullptr; it++) {
    const auto& extra = **it;
    switch (extra.type) {
      case PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK:
      case PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK:
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_TRACK:
      case PERFETTO_TE_HL_EXTRA_TYPE_NESTED_TRACKS:
        params.track = &extra;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP:
        custom_timestamp =
            &reinterpret_cast<const PerfettoTeHlExtraTimestamp&>(extra);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DYNAMIC_CATEGORY:
        params.dynamic_cat =
            reinterpret_cast<const PerfettoTeHlExtraDynamicCategory&>(extra)
                .desc;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_NO_INTERN:
        params.intern_name = false;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_FLUSH:
        params.flush = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS:
        params.has_proto_fields = true;
        break;
      default:
        break;
    }
  }

  if (params.track &&
      params.track->type == PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK) {
    const auto& named =
        reinterpret_cast<const PerfettoTeHlExtraNamedTrack&>(*params.track);
    params.named_track_uuid =
        PerfettoTeNamedTrackUuid(named.name, named.id, named.parent_uuid);
  }

  if (custom_timestamp) {
    params.timestamp.clock_id = custom_timestamp->timestamp.clock_id;
    params.timestamp.value = custom_timestamp->timestamp.value;
  } else {
    params.timestamp = TrackEventInternal::GetTraceTime();
  }

  return params;
}

// Appends the fields described by `fields` to `msg`.
void AppendHlProtoFields(TrackEventIncrementalState* incr,
                         protozero::Message* msg,
                         PerfettoTeHlProtoField* const* fields) {
  for (PerfettoTeHlProtoField* const* p = fields; *p != nullptr; p++) {
    switch ((*p)->type) {
      case PERFETTO_TE_HL_PROTO_TYPE_CSTR: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldCstr*>(*p);
        msg->AppendString(field->header.id, field->str);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_CSTR_INTERNED: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldCstrInterned*>(*p);
        PERFETTO_DCHECK(field->interned_type_id != 0);
        if (field->interned_type_id) {
          const char* str = field->str;
          size_t len = strlen(field->str);
          auto res = incr->iids.FindOrAssign(
              static_cast<int32_t>(field->interned_type_id), str, len);
          if (res.newly_assigned) {
            auto* ser = incr->serialized_interned_data
                            ->BeginNestedMessage<protozero::Message>(
                                field->interned_type_id);
            ser->AppendVarInt(kInternedStringIidFieldNumber, res.iid);
            ser->AppendString(kInternedStringNameFieldNumber, field->str);
          }
          msg->AppendVarInt(field->header.id, res.iid);
        }
        // If interned_type_id is zero, this is a user error, we drop the packet
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_BYTES: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldBytes*>(*p);
        msg->AppendBytes(field->header.id, field->buf, field->len);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_NESTED: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldNested*>(*p);
        auto* nested =
            msg->BeginNestedMessage<protozero::Message>(field->header.id);
        AppendHlProtoFields(incr, nested, field->fields);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_VARINT: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldVarInt*>(*p);
        msg->AppendVarInt(field->header.id, field->value);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_FIXED64: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldFixed64*>(*p);
        msg->AppendFixed(field->header.id, field->value);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_FIXED32: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldFixed32*>(*p);
        msg->AppendFixed(field->header.id, field->value);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_DOUBLE: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldDouble*>(*p);
        msg->AppendFixed(field->header.id, field->value);
        break;
      }
      case PERFETTO_TE_HL_PROTO_TYPE_FLOAT: {
        auto field = reinterpret_cast<PerfettoTeHlProtoFieldFloat*>(*p);
        msg->AppendFixed(field->header.id, field->value);
        break;
      }
    }
  }
}

// Interns `name` (if not null) as a debug annotation name and sets it on `dbg`.
void WriteDebugAnnotationName(TrackEventIncrementalState* incr,
                              protos::pbzero::DebugAnnotation* dbg,
                              const char* name) {
  if (!name) {
    return;
  }
  size_t len = strlen(name);
  auto res = incr->iids.FindOrAssign(
      protos::pbzero::InternedData::kDebugAnnotationNamesFieldNumber, name,
      len);
  if (res.newly_assigned) {
    auto* ser = incr->serialized_interned_data->add_debug_annotation_names();
    ser->set_iid(res.iid);
    ser->set_name(name, len);
  }
  dbg->set_name_iid(res.iid);
}

void WriteTrackEvent(TrackEventIncrementalState* incr,
                     protos::pbzero::TrackEvent* event,
                     const EventParams& params,
                     std::optional<uint64_t> track_uuid) {
  if (params.type != protos::pbzero::TrackEvent::TYPE_UNSPECIFIED) {
    event->set_type(params.type);
  }

  if (!params.dynamic_cat &&
      params.type != protos::pbzero::TrackEvent::TYPE_SLICE_END &&
      params.type != protos::pbzero::TrackEvent::TYPE_COUNTER) {
    uint64_t iid = params.cat->cat_iid;
    const bool newly_seen = incr->MarkCategorySeen(iid);
    if (newly_seen) {
      auto* ser = incr->serialized_interned_data->add_event_categories();
      ser->set_iid(iid);
      ser->set_name(params.cat->desc->name);
    }
    event->add_category_iids(iid);
  }

  if (params.name &&
      params.type != protos::pbzero::TrackEvent::TYPE_SLICE_END) {
    if (params.intern_name) {
      auto res = incr->iids.FindOrAssign(
          protos::pbzero::InternedData::kEventNamesFieldNumber, params.name,
          params.name_size);
      if (res.newly_assigned) {
        auto* ser = incr->serialized_interned_data->add_event_names();
        ser->set_iid(res.iid);
        ser->set_name(params.name, params.name_size);
      }
      event->set_name_iid(res.iid);
    } else {
      event->set_name(params.name, params.name_size);
    }
  }

  if (params.dynamic_cat &&
      params.type != protos::pbzero::TrackEvent::TYPE_SLICE_END &&
      params.type != protos::pbzero::TrackEvent::TYPE_COUNTER) {
    event->add_categories(params.dynamic_cat->name);
  }

  if (track_uuid) {
    event->set_track_uuid(*track_uuid);
  }

  for (const auto* it = params.extra_data; *it != nullptr; it++) {
    const auto& extra = **it;
    switch (extra.type) {
      case PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_INT64:
        if (params.type == protos::pbzero::TrackEvent::TYPE_COUNTER) {
          event->set_counter_value(
              reinterpret_cast<const PerfettoTeHlExtraCounterInt64&>(extra)
                  .value);
        }
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_DOUBLE:
        if (params.type == protos::pbzero::TrackEvent::TYPE_COUNTER) {
          event->set_double_counter_value(
              reinterpret_cast<const PerfettoTeHlExtraCounterDouble&>(extra)
                  .value);
        }
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL: {
        const auto& arg =
            reinterpret_cast<const PerfettoTeHlExtraDebugArgBool&>(extra);
        auto* dbg = event->add_debug_annotations();
        dbg->set_bool_value(arg.value);
        WriteDebugAnnotationName(incr, dbg, arg.name);
        break;
      }
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64: {
        const auto& arg =
            reinterpret_cast<const PerfettoTeHlExtraDebugArgUint64&>(extra);
        auto* dbg = event->add_debug_annotations();
        dbg->set_uint_value(arg.value);
        WriteDebugAnnotationName(incr, dbg, arg.name);
        break;
      }
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64: {
        const auto& arg =
            reinterpret_cast<const PerfettoTeHlExtraDebugArgInt64&>(extra);
        auto* dbg = event->add_debug_annotations();
        dbg->set_int_value(arg.value);
        WriteDebugAnnotationName(incr, dbg, arg.name);
        break;
      }
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE: {
        const auto& arg =
            reinterpret_cast<const PerfettoTeHlExtraDebugArgDouble&>(extra);
        auto* dbg = event->add_debug_annotations();
        dbg->set_double_value(arg.value);
        WriteDebugAnnotationName(incr, dbg, arg.name);
        break;
      }
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING: {
        const auto& arg =
            reinterpret_cast<const PerfettoTeHlExtraDebugArgString&>(extra);
        auto* dbg = event->add_debug_annotations();
        dbg->set_string_value(arg.value);
        WriteDebugAnnotationName(incr, dbg, arg.name);
        break;
      }
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER: {
        const auto& arg =
            reinterpret_cast<const PerfettoTeHlExtraDebugArgPointer&>(extra);
        auto* dbg = event->add_debug_annotations();
        dbg->set_pointer_value(arg.value);
        WriteDebugAnnotationName(incr, dbg, arg.name);
        break;
      }
      case PERFETTO_TE_HL_EXTRA_TYPE_FLOW:
        event->add_flow_ids(
            reinterpret_cast<const PerfettoTeHlExtraFlow&>(extra).id);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_TERMINATING_FLOW:
        event->add_terminating_flow_ids(
            reinterpret_cast<const PerfettoTeHlExtraFlow&>(extra).id);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID:
        event->set_correlation_id(
            reinterpret_cast<const PerfettoTeHlExtraCorrelationId&>(extra).id);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID_STR:
        event->set_correlation_id_str(
            reinterpret_cast<const PerfettoTeHlExtraCorrelationIdStr&>(extra)
                .str);
        break;
      default:
        break;
    }
  }

  // Custom fields go last so they can override the built-in fields.
  if (params.has_proto_fields) {
    for (const auto* it = params.extra_data; *it != nullptr; it++) {
      const auto& extra = **it;
      if (extra.type == PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS) {
        const auto* fields =
            reinterpret_cast<const PerfettoTeHlExtraProtoFields&>(extra).fields;
        AppendHlProtoFields(incr, event, fields);
      }
    }
  }
}

// Emits (once per sequence) the track descriptor for the named track described
// by `track` (its `header` is ignored) and returns `uuid`.
// `uuid` must be PerfettoTeNamedTrackUuid(track.name, track.id, parent_uuid)
// and is passed in so that callers can compute it once per event.
uint64_t EmitNamedTrack(uint64_t uuid,
                        uint64_t parent_uuid,
                        const PerfettoTeHlNestedTrackNamed& track,
                        perfetto::shlib::TrackEventIncrementalState* incr_state,
                        perfetto::TraceWriterBase* trace_writer) {
  if (incr_state->seen_track_uuids.insert(uuid).second) {
    PERFETTO_DCHECK(
        uuid == PerfettoTeNamedTrackUuid(track.name, track.id, parent_uuid));
    auto packet = trace_writer->NewTracePacket();
    auto* track_descriptor = packet->set_track_descriptor();
    track_descriptor->set_uuid(uuid);
    if (parent_uuid) {
      track_descriptor->set_parent_uuid(parent_uuid);
    }
    if (track.is_name_static) {
      track_descriptor->set_static_name(track.name);
    } else {
      track_descriptor->set_name(track.name);
    }
    if (track.sibling_order_rank != 0) {
      track_descriptor->set_sibling_order_rank(track.sibling_order_rank);
    }
    if (track.child_ordering != PERFETTO_TE_HL_CHILD_ORDERING_UNKNOWN) {
      track_descriptor->set_child_ordering(
          static_cast<protos::pbzero::TrackDescriptor::ChildTracksOrdering>(
              track.child_ordering));
    }
    if (track.sibling_merge_behavior !=
        PERFETTO_TE_HL_SIBLING_MERGE_BEHAVIOR_UNSPECIFIED) {
      track_descriptor->set_sibling_merge_behavior(
          static_cast<protos::pbzero::TrackDescriptor::SiblingMergeBehavior>(
              track.sibling_merge_behavior));
    }
    if (track.sibling_merge_behavior ==
        PERFETTO_TE_HL_SIBLING_MERGE_BEHAVIOR_BY_SIBLING_MERGE_KEY) {
      if (track.sibling_merge_key_str) {
        track_descriptor->set_sibling_merge_key(track.sibling_merge_key_str);
      } else {
        track_descriptor->set_sibling_merge_key_int(
            track.sibling_merge_key_int);
      }
    }
  }
  return uuid;
}

uint64_t EmitRegisteredTrack(
    const PerfettoTeRegisteredTrackImpl* registered_track,
    perfetto::shlib::TrackEventIncrementalState* incr_state,
    perfetto::TraceWriterBase* trace_writer) {
  if (incr_state->seen_track_uuids.insert(registered_track->uuid).second) {
    auto packet = trace_writer->NewTracePacket();
    auto* track_descriptor = packet->set_track_descriptor();
    track_descriptor->AppendRawProtoBytes(registered_track->descriptor,
                                          registered_track->descriptor_size);
  }
  return registered_track->uuid;
}

uint64_t EmitProtoTrack(uint64_t uuid,
                        PerfettoTeHlProtoField* const* fields,
                        perfetto::shlib::TrackEventIncrementalState* incr_state,
                        perfetto::TraceWriterBase* trace_writer) {
  if (incr_state->seen_track_uuids.insert(uuid).second) {
    auto packet = trace_writer->NewTracePacket();
    auto* track_descriptor = packet->set_track_descriptor();
    track_descriptor->set_uuid(uuid);
    AppendHlProtoFields(incr_state, track_descriptor, fields);
  }
  return uuid;
}

uint64_t EmitProtoTrackWithParentUuid(
    uint64_t uuid,
    uint64_t parent_uuid,
    PerfettoTeHlProtoField* const* fields,
    perfetto::shlib::TrackEventIncrementalState* incr_state,
    perfetto::TraceWriterBase* trace_writer) {
  if (incr_state->seen_track_uuids.insert(uuid).second) {
    auto packet = trace_writer->NewTracePacket();
    auto* track_descriptor = packet->set_track_descriptor();
    track_descriptor->set_uuid(uuid);
    track_descriptor->set_parent_uuid(parent_uuid);
    AppendHlProtoFields(incr_state, track_descriptor, fields);
  }
  return uuid;
}

// Emits (once per sequence) the descriptors of the track selected by
// `params.track` and returns its uuid, or nullopt for the default track.
std::optional<uint64_t> EmitTrack(const EventParams& params,
                                  TrackEventIncrementalState* incr_state,
                                  TraceWriterBase* trace_writer) {
  if (!params.track) {
    return std::nullopt;
  }
  switch (params.track->type) {
    case PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK: {
      const auto& extra =
          reinterpret_cast<const PerfettoTeHlExtraRegisteredTrack&>(
              *params.track);
      return EmitRegisteredTrack(extra.track, incr_state, trace_writer);
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK: {
      const auto& extra =
          reinterpret_cast<const PerfettoTeHlExtraNamedTrack&>(*params.track);
      PerfettoTeHlNestedTrackNamed named{};
      named.name = extra.name;
      named.id = extra.id;
      named.is_name_static = extra.is_name_static;
      return EmitNamedTrack(params.named_track_uuid, extra.parent_uuid, named,
                            incr_state, trace_writer);
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_TRACK: {
      const auto& extra =
          reinterpret_cast<const PerfettoTeHlExtraProtoTrack&>(*params.track);
      return EmitProtoTrack(extra.uuid, extra.fields, incr_state, trace_writer);
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_NESTED_TRACKS: {
      const auto& extra =
          reinterpret_cast<const PerfettoTeHlExtraNestedTracks&>(*params.track);
      uint64_t uuid = 0;
      for (PerfettoTeHlNestedTrack* const* tp = extra.tracks; *tp != nullptr;
           tp++) {
        auto track_type =
            static_cast<enum PerfettoTeHlNestedTrackType>((*tp)->type);

        switch (track_type) {
          case PERFETTO_TE_HL_NESTED_TRACK_TYPE_NAMED: {
            auto* named_track =
                reinterpret_cast<PerfettoTeHlNestedTrackNamed*>(*tp);
            uint64_t parent_uuid = uuid;
            uuid = EmitNamedTrack(
                PerfettoTeNamedTrackUuid(named_track->name, named_track->id,
                                         parent_uuid),
                parent_uuid, *named_track, incr_state, trace_writer);
            break;
          }
          case PERFETTO_TE_HL_NESTED_TRACK_TYPE_PROCESS: {
            uuid = perfetto_te_process_track_uuid;
            break;
          }
          case PERFETTO_TE_HL_NESTED_TRACK_TYPE_THREAD: {
            uuid = incr_state->thread_track_uuid;
            break;
          }
          case PERFETTO_TE_HL_NESTED_TRACK_TYPE_PROTO: {
            auto* proto_track =
                reinterpret_cast<PerfettoTeHlNestedTrackProto*>(*tp);
            uuid = EmitProtoTrackWithParentUuid(proto_track->id ^ uuid, uuid,
                                                proto_track->fields, incr_state,
                                                trace_writer);
            break;
          }
          case PERFETTO_TE_HL_NESTED_TRACK_TYPE_REGISTERED: {
            auto* registered_track =
                reinterpret_cast<PerfettoTeHlNestedTrackRegistered*>(*tp);
            uuid = EmitRegisteredTrack(registered_track->track, incr_state,
                                       trace_writer);
            break;
          }
        }
      }
      return uuid;
    }
    default:
      return std::nullopt;
  }
}

// Writes the event to the instance `ii`, unless the instance disables
// `params.dynamic_cat`.
void InstanceOp(internal::DataSourceType* ds,
                const internal::DataSourceType::InstancesIterator* ii,
                const EventParams& params) {
  auto* incr_state = static_cast<TrackEventIncrementalState*>(
      ds->GetIncrementalState(ii->instance, ii->i));
  if (PERFETTO_UNLIKELY(params.dynamic_cat) &&
      !TrackEvent::IsDynamicCategoryEnabled(ii->i, incr_state,
                                            *params.dynamic_cat)) {
    return;
  }

  perfetto::TraceWriterBase* trace_writer = ii->instance->trace_writer.get();

  const auto& track_event_tls = *static_cast<TrackEventTlsState*>(
      ii->instance->data_source_custom_tls.get());

  ResetIncrementalStateIfRequired(trace_writer, incr_state, track_event_tls,
                                  params.timestamp);

  std::optional<uint64_t> track_uuid =
      EmitTrack(params, incr_state, trace_writer);

  {
    auto packet = NewTracePacketInternal(
        trace_writer, incr_state, track_event_tls, params.timestamp,
        protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE);
    auto* track_event = packet->set_track_event();
    WriteTrackEvent(incr_state, track_event, params, track_uuid);
    track_event->Finalize();

    if (!incr_state->serialized_interned_data.empty()) {
      auto ranges = incr_state->serialized_interned_data.GetRanges();
      packet->AppendScatteredBytes(
          protos::pbzero::TracePacket::kInternedDataFieldNumber, ranges.data(),
          ranges.size());
      incr_state->serialized_interned_data.Reset();
    }
  }

  if (PERFETTO_UNLIKELY(params.flush)) {
    trace_writer->Flush();
  }
}

// Iterates over the track event data source instances that enable `cat`.
// Runs the data source prologue in the constructor and the epilogue in the
// destructor if the prologue succeeded.
// If current() is null right after construction, callers can skip all per-event
// work.
// Keep prologue, iteration, and epilogue rules in sync with
// DataSource::TraceWithInstances().
class ActiveInstances {
 public:
  using Iterator = internal::DataSourceType::InstancesIterator;

  explicit ActiveInstances(PerfettoTeCategoryImpl* cat) : cat_(cat) {
    uint32_t cached_instances =
        TracePointTraits::GetActiveInstances({cat})->load(
            std::memory_order_relaxed);
    if (PERFETTO_UNLIKELY(!cached_instances)) {
      return;
    }
    internal::DataSourceThreadLocalState** tls_state =
        TrackEvent::GetTlsState();
    if (!ds_->TracePrologue<TrackEventDataSourceTraits, TracePointTraits>(
            tls_state, &cached_instances, {cat})) {
      return;
    }
    tls_state_ = *tls_state;
    it_ = ds_->BeginIteration<TracePointTraits>(cached_instances, tls_state_,
                                                {cat});
  }

  ~ActiveInstances() {
    if (tls_state_) {
      ds_->TraceEpilogue(tls_state_);
    }
  }

  ActiveInstances(const ActiveInstances&) = delete;
  ActiveInstances& operator=(const ActiveInstances&) = delete;

  // The current instance, or nullptr when there is none left.
  const Iterator* current() const { return it_.instance ? &it_ : nullptr; }

  // Advances to the next instance and returns it, or nullptr if there is none.
  const Iterator* Next() {
    ds_->NextIteration<TracePointTraits>(&it_, tls_state_, {cat_});
    return current();
  }

  internal::DataSourceType* ds() const { return ds_; }

 private:
  PerfettoTeCategoryImpl* const cat_;
  internal::DataSourceType* const ds_ = TrackEvent::GetType();
  // Set only if the prologue succeeded.
  internal::DataSourceThreadLocalState* tls_state_ = nullptr;
  Iterator it_{};
};

void TeHlEmit(PerfettoTeCategoryImpl* cat,
              int32_t type,
              const char* name,
              PerfettoTeHlExtra* const* extra_data) {
  ActiveInstances instances(cat);
  if (!instances.current()) {
    return;
  }
  const EventParams params =
      PrepareEvent(cat, type, name, name ? strlen(name) : 0, extra_data);
  for (const auto* ii = instances.current(); ii; ii = instances.Next()) {
    InstanceOp(instances.ds(), ii, params);
  }
}

}  // namespace
}  // namespace perfetto::shlib

void PerfettoTeHlEmitImpl(struct PerfettoTeCategoryImpl* cat,
                          int32_t type,
                          const char* name,
                          struct PerfettoTeHlExtra* const* extra_data) {
  perfetto::shlib::TeHlEmit(cat, type, name, extra_data);
}
