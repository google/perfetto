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

using TrackSelection = std::variant<std::monostate,
                                    const PerfettoTeRegisteredTrackImpl*,
                                    const PerfettoTeHlExtraNamedTrack*,
                                    const PerfettoTeHlExtraProtoTrack*,
                                    const PerfettoTeHlExtraNestedTracks*>;

// Shared by all recordings of this event. Pointers borrow the caller's storage
// until TeHlEmit() returns.
struct EventParams {
  PerfettoTeCategoryImpl* cat = nullptr;
  protos::pbzero::TrackEvent::Type type =
      protos::pbzero::TrackEvent::TYPE_UNSPECIFIED;
  const char* name = nullptr;
  size_t name_len = 0;
  TraceTimestamp timestamp{};

  TrackSelection track;
  // Cached after a recording accepts the category.
  bool name_and_track_cached = false;
  uint64_t named_track_uuid = 0;
  const PerfettoTeCategoryDescriptor* dynamic_cat = nullptr;
  bool use_interning = true;
  bool flush = false;

  const PerfettoTeHlExtra* const* extra_data = nullptr;
  bool has_builtin_fields = false;
  bool has_proto_fields = false;
};

EventParams PrepareEvent(PerfettoTeCategoryImpl* cat,
                         int32_t type,
                         const char* name,
                         const PerfettoTeHlExtra* const* extra_data) {
  EventParams params;
  params.cat = cat;
  params.type = EventType(type);
  params.name = name;
  params.extra_data = extra_data;

  const PerfettoTeHlExtraTimestamp* custom_timestamp = nullptr;
  for (const auto* it = extra_data; *it != nullptr; it++) {
    const auto& extra = **it;
    switch (extra.type) {
      case PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK:
        params.track =
            reinterpret_cast<const PerfettoTeHlExtraRegisteredTrack&>(extra)
                .track;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK:
        params.track =
            &reinterpret_cast<const PerfettoTeHlExtraNamedTrack&>(extra);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_TRACK:
        params.track =
            &reinterpret_cast<const PerfettoTeHlExtraProtoTrack&>(extra);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_NESTED_TRACKS:
        params.track =
            &reinterpret_cast<const PerfettoTeHlExtraNestedTracks&>(extra);
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
        params.use_interning = false;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_FLUSH:
        params.flush = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_INT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_DOUBLE:
        if (params.type == protos::pbzero::TrackEvent::TYPE_COUNTER)
          params.has_builtin_fields = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER:
      case PERFETTO_TE_HL_EXTRA_TYPE_FLOW:
      case PERFETTO_TE_HL_EXTRA_TYPE_TERMINATING_FLOW:
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID:
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID_STR:
        params.has_builtin_fields = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS:
        params.has_proto_fields = true;
        break;
      default:
        break;
    }
  }

  // Every recording gets the same event timestamp.
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

void WriteTrackEvent(TrackEventIncrementalState* incr,
                     protos::pbzero::TrackEvent* event,
                     const EventParams& params,
                     std::optional<uint64_t> track_uuid) {
  if (params.type != protos::pbzero::TrackEvent::TYPE_UNSPECIFIED)
    event->set_type(params.type);

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
    if (params.use_interning) {
      auto res = incr->iids.FindOrAssign(
          protos::pbzero::InternedData::kEventNamesFieldNumber, params.name,
          params.name_len);
      if (res.newly_assigned) {
        auto* ser = incr->serialized_interned_data->add_event_names();
        ser->set_iid(res.iid);
        ser->set_name(params.name, params.name_len);
      }
      event->set_name_iid(res.iid);
    } else {
      event->set_name(params.name, params.name_len);
    }
  }

  if (params.dynamic_cat &&
      params.type != protos::pbzero::TrackEvent::TYPE_SLICE_END &&
      params.type != protos::pbzero::TrackEvent::TYPE_COUNTER) {
    event->add_categories(params.dynamic_cat->name);
  }

  if (track_uuid)
    event->set_track_uuid(*track_uuid);

  if (params.has_builtin_fields) {
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
        case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL:
        case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64:
        case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64:
        case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE:
        case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING:
        case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER: {
          auto* dbg = event->add_debug_annotations();
          const char* arg_name = nullptr;
          if (extra.type == PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL) {
            const auto& arg =
                reinterpret_cast<const PerfettoTeHlExtraDebugArgBool&>(extra);
            dbg->set_bool_value(arg.value);
            arg_name = arg.name;
          } else if (extra.type == PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64) {
            const auto& arg =
                reinterpret_cast<const PerfettoTeHlExtraDebugArgUint64&>(extra);
            dbg->set_uint_value(arg.value);
            arg_name = arg.name;
          } else if (extra.type == PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64) {
            const auto& arg =
                reinterpret_cast<const PerfettoTeHlExtraDebugArgInt64&>(extra);
            dbg->set_int_value(arg.value);
            arg_name = arg.name;
          } else if (extra.type == PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE) {
            const auto& arg =
                reinterpret_cast<const PerfettoTeHlExtraDebugArgDouble&>(extra);
            dbg->set_double_value(arg.value);
            arg_name = arg.name;
          } else if (extra.type == PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING) {
            const auto& arg =
                reinterpret_cast<const PerfettoTeHlExtraDebugArgString&>(extra);
            dbg->set_string_value(arg.value);
            arg_name = arg.name;
          } else if (extra.type ==
                     PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER) {
            const auto& arg =
                reinterpret_cast<const PerfettoTeHlExtraDebugArgPointer&>(
                    extra);
            dbg->set_pointer_value(arg.value);
            arg_name = arg.name;
          }

          if (arg_name != nullptr) {
            size_t len = strlen(arg_name);
            auto res = incr->iids.FindOrAssign(
                protos::pbzero::InternedData::kDebugAnnotationNamesFieldNumber,
                arg_name, len);
            if (res.newly_assigned) {
              auto* ser =
                  incr->serialized_interned_data->add_debug_annotation_names();
              ser->set_iid(res.iid);
              ser->set_name(arg_name, len);
            }
            dbg->set_name_iid(res.iid);
          }
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
              reinterpret_cast<const PerfettoTeHlExtraCorrelationId&>(extra)
                  .id);
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
// by `track` (its `header` is ignored).
void EmitNamedTrack(uint64_t uuid,
                    uint64_t parent_uuid,
                    const PerfettoTeHlNestedTrackNamed& track,
                    perfetto::shlib::TrackEventIncrementalState* incr_state,
                    perfetto::TraceWriterBase* trace_writer) {
  if (incr_state->seen_track_uuids.insert(uuid).second) {
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

// If the category `dyn_cat` is enabled on the data source instance pointed by
// `ii`, returns immediately. Otherwise, advances `ii` to a data source instance
// where `dyn_cat` is enabled. If there's no data source instance where
// `dyn_cat` is enabled, `ii->instance` will be nullptr.
void AdvanceToFirstEnabledDynamicCategory(
    internal::DataSourceType::InstancesIterator* ii,
    internal::DataSourceThreadLocalState* tls_state,
    struct PerfettoTeCategoryImpl* cat,
    const PerfettoTeCategoryDescriptor& dyn_cat) {
  internal::DataSourceType* ds = shlib::TrackEvent::GetType();
  for (; ii->instance; ds->NextIteration</*Traits=*/shlib::TracePointTraits>(
           ii, tls_state, {cat})) {
    auto* incr_state = static_cast<TrackEventIncrementalState*>(
        ds->GetIncrementalState(ii->instance, ii->i));
    if (shlib::TrackEvent::IsDynamicCategoryEnabled(ii->i, incr_state,
                                                    dyn_cat)) {
      break;
    }
  }
}

void InstanceOp(internal::DataSourceType* ds,
                internal::DataSourceType::InstancesIterator* ii,
                internal::DataSourceThreadLocalState* tls_state,
                EventParams* params) {
  if (!ii->instance)
    return;

  if (PERFETTO_UNLIKELY(params->dynamic_cat)) {
    AdvanceToFirstEnabledDynamicCategory(ii, tls_state, params->cat,
                                         *params->dynamic_cat);
    if (!ii->instance)
      return;
  }

  // Rejecting every session must not scan the event or track names.
  if (!params->name_and_track_cached) {
    if (params->name &&
        params->type != protos::pbzero::TrackEvent::TYPE_SLICE_END) {
      params->name_len = strlen(params->name);
    }
    if (const auto* track =
            std::get_if<const PerfettoTeHlExtraNamedTrack*>(&params->track)) {
      params->named_track_uuid = PerfettoTeNamedTrackUuid(
          (*track)->name, (*track)->id, (*track)->parent_uuid);
    }
    params->name_and_track_cached = true;
  }

  perfetto::TraceWriterBase* trace_writer = ii->instance->trace_writer.get();

  const auto& track_event_tls = *static_cast<TrackEventTlsState*>(
      ii->instance->data_source_custom_tls.get());

  auto* incr_state = static_cast<TrackEventIncrementalState*>(
      ds->GetIncrementalState(ii->instance, ii->i));
  ResetIncrementalStateIfRequired(trace_writer, incr_state, track_event_tls,
                                  params->timestamp);

  const auto& track = params->track;
  std::optional<uint64_t> track_uuid;
  if (std::holds_alternative<const PerfettoTeRegisteredTrackImpl*>(track)) {
    auto* registered_track =
        std::get<const PerfettoTeRegisteredTrackImpl*>(track);
    track_uuid =
        EmitRegisteredTrack(registered_track, incr_state, trace_writer);
  } else if (std::holds_alternative<const PerfettoTeHlExtraNamedTrack*>(
                 track)) {
    auto* named_track = std::get<const PerfettoTeHlExtraNamedTrack*>(track);
    PerfettoTeHlNestedTrackNamed named{};
    named.name = named_track->name;
    named.is_name_static = named_track->is_name_static;
    track_uuid = params->named_track_uuid;
    EmitNamedTrack(*track_uuid, named_track->parent_uuid, named, incr_state,
                   trace_writer);
  } else if (std::holds_alternative<const PerfettoTeHlExtraProtoTrack*>(
                 track)) {
    auto* proto_track = std::get<const PerfettoTeHlExtraProtoTrack*>(track);
    track_uuid = EmitProtoTrack(proto_track->uuid, proto_track->fields,
                                incr_state, trace_writer);
  } else if (std::holds_alternative<const PerfettoTeHlExtraNestedTracks*>(
                 track)) {
    auto* nested = std::get<const PerfettoTeHlExtraNestedTracks*>(track);

    uint64_t uuid = 0;

    for (PerfettoTeHlNestedTrack* const* tp = nested->tracks; *tp != nullptr;
         tp++) {
      auto track_type =
          static_cast<enum PerfettoTeHlNestedTrackType>((*tp)->type);

      switch (track_type) {
        case PERFETTO_TE_HL_NESTED_TRACK_TYPE_NAMED: {
          auto* named_track =
              reinterpret_cast<PerfettoTeHlNestedTrackNamed*>(*tp);
          uint64_t parent_uuid = uuid;
          uuid = PerfettoTeNamedTrackUuid(named_track->name, named_track->id,
                                          parent_uuid);
          EmitNamedTrack(uuid, parent_uuid, *named_track, incr_state,
                         trace_writer);
        } break;
        case PERFETTO_TE_HL_NESTED_TRACK_TYPE_PROCESS: {
          uuid = perfetto_te_process_track_uuid;
        } break;
        case PERFETTO_TE_HL_NESTED_TRACK_TYPE_THREAD: {
          uuid = incr_state->thread_track_uuid;
        } break;
        case PERFETTO_TE_HL_NESTED_TRACK_TYPE_PROTO: {
          auto* proto_track =
              reinterpret_cast<PerfettoTeHlNestedTrackProto*>(*tp);
          uuid = EmitProtoTrackWithParentUuid(proto_track->id ^ uuid, uuid,
                                              proto_track->fields, incr_state,
                                              trace_writer);
        } break;
        case PERFETTO_TE_HL_NESTED_TRACK_TYPE_REGISTERED: {
          auto* registered_track =
              reinterpret_cast<PerfettoTeHlNestedTrackRegistered*>(*tp);
          uuid = EmitRegisteredTrack(registered_track->track, incr_state,
                                     trace_writer);
        } break;
      }
    }
    track_uuid = uuid;
  }

  {
    auto packet = NewTracePacketInternal(
        trace_writer, incr_state, track_event_tls, params->timestamp,
        protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE);
    auto* track_event = packet->set_track_event();
    WriteTrackEvent(incr_state, track_event, *params, track_uuid);
    track_event->Finalize();

    if (!incr_state->serialized_interned_data.empty()) {
      auto ranges = incr_state->serialized_interned_data.GetRanges();
      packet->AppendScatteredBytes(
          protos::pbzero::TracePacket::kInternedDataFieldNumber, ranges.data(),
          ranges.size());
      incr_state->serialized_interned_data.Reset();
    }
  }

  if (PERFETTO_UNLIKELY(params->flush)) {
    trace_writer->Flush();
  }
}

void TeHlEmit(struct PerfettoTeCategoryImpl* cat,
              int32_t type,
              const char* name,
              struct PerfettoTeHlExtra* const* extra_data) {
  uint32_t cached_instances =
      perfetto::shlib::TracePointTraits::GetActiveInstances({cat})->load(
          std::memory_order_relaxed);
  if (!cached_instances) {
    return;
  }

  perfetto::internal::DataSourceType* ds =
      perfetto::shlib::TrackEvent::GetType();

  perfetto::internal::DataSourceThreadLocalState*& tls_state =
      *perfetto::shlib::TrackEvent::GetTlsState();

  if (!ds->TracePrologue<perfetto::shlib::TrackEventDataSourceTraits,
                         perfetto::shlib::TracePointTraits>(
          &tls_state, &cached_instances, {cat})) {
    return;
  }

  auto ii = ds->BeginIteration<perfetto::shlib::TracePointTraits>(
      cached_instances, tls_state, {cat});

  EventParams params = PrepareEvent(cat, type, name, extra_data);

  for (; ii.instance;
       ds->NextIteration</*Traits=*/perfetto::shlib::TracePointTraits>(
           &ii, tls_state, {cat})) {
    perfetto::shlib::InstanceOp(ds, &ii, tls_state, &params);
  }
  ds->TraceEpilogue(tls_state);
}

}  // namespace
}  // namespace perfetto::shlib

void PerfettoTeHlEmitImpl(struct PerfettoTeCategoryImpl* cat,
                          int32_t type,
                          const char* name,
                          struct PerfettoTeHlExtra* const* extra_data) {
  perfetto::shlib::TeHlEmit(cat, type, name, extra_data);
}
