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

#include <string.h>

#include <memory>
#include <optional>

#include "perfetto/base/endian.h"
#include "perfetto/protozero/proto_utils.h"
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
          auto res = incr->iids.FindOrAssignString(
              static_cast<int32_t>(field->interned_type_id), field->str);
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

enum class TrackKind : uint8_t {
  kDefault,
  kRegistered,
  kNamed,
  kProto,
  kNested,
};

// One PERFETTO_TE event: the arguments of PerfettoTeHlEmitImpl() and what
// ParseEvent() extracts from its extras. It is the same for every data source
// instance, so it is computed once per event.
struct HlEvent {
  PerfettoTeCategoryImpl* cat = nullptr;
  protos::pbzero::TrackEvent::Type type =
      protos::pbzero::TrackEvent::TYPE_UNSPECIFIED;
  const char* name = nullptr;
  const PerfettoTeHlExtra* const* extras = nullptr;
  TraceTimestamp ts{};

  TrackKind track_kind = TrackKind::kDefault;
  // Depends on `track_kind`: PerfettoTeRegisteredTrackImpl,
  // PerfettoTeHlExtraNamedTrack, PerfettoTeHlExtraProtoTrack or
  // PerfettoTeHlExtraNestedTracks.
  const void* track = nullptr;
  const PerfettoTeCategoryDescriptor* dynamic_cat = nullptr;
  bool no_intern = false;
  bool flush = false;
  bool has_proto_fields = false;
  // Upper bound of the bytes that EncodeTrackEventFields() writes.
  size_t fields_size_bound = 0;
};

// Upper bounds used by ParseEvent() to compute `fields_size_bound`.
// The tags of the TrackEvent and DebugAnnotation fields take up to two bytes.
constexpr size_t kMaxVarIntFieldSize = 2 + 10;
// A string field without its content: tag and length.
constexpr size_t kMaxStringFieldOverhead = 2 + 5;
// A debug annotation with a varint value: tag, one byte of length, the value
// field and the name iid field. Counters, flows and correlation ids are
// smaller.
constexpr size_t kMaxExtraSize = 1 + 1 + 2 * kMaxVarIntFieldSize;
// A string can make a one byte length grow to five bytes.
constexpr size_t kMaxLengthGrowth = 4;

// Parses the extras of the event and reads the clock, unless the event has a
// custom timestamp.
HlEvent ParseEvent(PerfettoTeCategoryImpl* cat,
                   int32_t type,
                   const char* name,
                   const PerfettoTeHlExtra* const* extras) {
  HlEvent ev;
  ev.cat = cat;
  ev.type = EventType(type);
  ev.name = name;
  ev.extras = extras;

  // Type, category iid, name iid and track uuid.
  size_t bound = 4 * kMaxVarIntFieldSize;
  const PerfettoTeHlExtraTimestamp* custom_timestamp = nullptr;
  for (const auto* it = extras; *it != nullptr; it++) {
    const struct PerfettoTeHlExtra& extra = **it;
    switch (extra.type) {
      case PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK:
        ev.track_kind = TrackKind::kRegistered;
        ev.track =
            reinterpret_cast<const struct PerfettoTeHlExtraRegisteredTrack&>(
                extra)
                .track;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK:
        ev.track_kind = TrackKind::kNamed;
        ev.track = &extra;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_TRACK:
        ev.track_kind = TrackKind::kProto;
        ev.track = &extra;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_NESTED_TRACKS:
        ev.track_kind = TrackKind::kNested;
        ev.track = &extra;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP:
        custom_timestamp =
            &reinterpret_cast<const struct PerfettoTeHlExtraTimestamp&>(extra);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DYNAMIC_CATEGORY:
        ev.dynamic_cat =
            reinterpret_cast<const struct PerfettoTeHlExtraDynamicCategory&>(
                extra)
                .desc;
        bound += kMaxStringFieldOverhead + strlen(ev.dynamic_cat->name);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_NO_INTERN:
        ev.no_intern = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_FLUSH:
        ev.flush = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS:
        ev.has_proto_fields = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING:
        bound +=
            kMaxExtraSize + kMaxLengthGrowth +
            strlen(
                reinterpret_cast<const struct PerfettoTeHlExtraDebugArgString&>(
                    extra)
                    .value);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID_STR:
        bound +=
            kMaxStringFieldOverhead +
            strlen(reinterpret_cast<
                       const struct PerfettoTeHlExtraCorrelationIdStr&>(extra)
                       .str);
        break;
      default:
        bound += kMaxExtraSize;
        break;
    }
  }
  if (ev.no_intern && name)
    bound += kMaxStringFieldOverhead + strlen(name);
  ev.fields_size_bound = bound;

  if (custom_timestamp) {
    ev.ts.clock_id = custom_timestamp->timestamp.clock_id;
    ev.ts.value = custom_timestamp->timestamp.value;
  } else {
    ev.ts = TrackEventInternal::GetTraceTime();
  }
  return ev;
}

// Raw protobuf encoding into a buffer that the caller sized.

using protozero::proto_utils::MakeTagFixed;
using protozero::proto_utils::MakeTagLengthDelimited;
using protozero::proto_utils::MakeTagVarInt;
using protozero::proto_utils::WriteVarInt;

uint8_t* WriteVarIntField(uint32_t field_id, uint64_t value, uint8_t* buf) {
  buf = WriteVarInt(MakeTagVarInt(field_id), buf);
  return WriteVarInt(value, buf);
}

uint8_t* WriteFixed64Field(uint32_t field_id, uint64_t value, uint8_t* buf) {
  buf = WriteVarInt(MakeTagFixed<uint64_t>(field_id), buf);
  value = base::HostToLE64(value);
  memcpy(buf, &value, sizeof(value));
  return buf + sizeof(value);
}

uint8_t* WriteDoubleField(uint32_t field_id, double value, uint8_t* buf) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return WriteFixed64Field(field_id, bits, buf);
}

uint8_t* WriteStringField(uint32_t field_id, const char* str, uint8_t* buf) {
  const size_t len = strlen(str);
  buf = WriteVarInt(MakeTagLengthDelimited(field_id), buf);
  buf = WriteVarInt(len, buf);
  memcpy(buf, str, len);
  return buf + len;
}

// A nested message is written in place, so its length is only known at the
// end:
//   uint8_t* payload = BeginNestedField(field_id, buf);
//   uint8_t* end = ... write the payload at `payload` ...;
//   end = EndNestedField(payload, end);
// The length gets one byte. If the payload is 128 bytes or more,
// EndNestedField() moves it to make room for a longer length.
uint8_t* BeginNestedField(uint32_t field_id, uint8_t* buf) {
  return WriteVarInt(MakeTagLengthDelimited(field_id), buf) + 1;
}

uint8_t* EndNestedField(uint8_t* payload, uint8_t* end) {
  const size_t size = static_cast<size_t>(end - payload);
  if (PERFETTO_LIKELY(size < 128)) {
    payload[-1] = static_cast<uint8_t>(size);
    return end;
  }
  uint8_t length[10];
  const size_t length_size =
      static_cast<size_t>(WriteVarInt(size, length) - length);
  memmove(payload + length_size - 1, payload, size);
  memcpy(payload - 1, length, length_size);
  return end + length_size - 1;
}

// Encodes the DebugAnnotation of a PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_*
// extra: the value, then the interned name.
uint8_t* EncodeDebugAnnotation(const struct PerfettoTeHlExtra& extra,
                               TrackEventIncrementalState* incr,
                               uint8_t* buf) {
  using protos::pbzero::DebugAnnotation;
  uint8_t* payload = BeginNestedField(
      protos::pbzero::TrackEvent::kDebugAnnotationsFieldNumber, buf);
  uint8_t* end = payload;
  const char* name = nullptr;
  switch (extra.type) {
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL: {
      const auto& arg =
          reinterpret_cast<const struct PerfettoTeHlExtraDebugArgBool&>(extra);
      end = WriteVarIntField(DebugAnnotation::kBoolValueFieldNumber,
                             arg.value ? 1 : 0, end);
      name = arg.name;
      break;
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64: {
      const auto& arg =
          reinterpret_cast<const struct PerfettoTeHlExtraDebugArgUint64&>(
              extra);
      end = WriteVarIntField(DebugAnnotation::kUintValueFieldNumber, arg.value,
                             end);
      name = arg.name;
      break;
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64: {
      const auto& arg =
          reinterpret_cast<const struct PerfettoTeHlExtraDebugArgInt64&>(extra);
      // A negative int64 takes 10 bytes, like protozero writes it.
      end = WriteVarIntField(DebugAnnotation::kIntValueFieldNumber,
                             static_cast<uint64_t>(arg.value), end);
      name = arg.name;
      break;
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE: {
      const auto& arg =
          reinterpret_cast<const struct PerfettoTeHlExtraDebugArgDouble&>(
              extra);
      end = WriteDoubleField(DebugAnnotation::kDoubleValueFieldNumber,
                             arg.value, end);
      name = arg.name;
      break;
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING: {
      const auto& arg =
          reinterpret_cast<const struct PerfettoTeHlExtraDebugArgString&>(
              extra);
      end = WriteStringField(DebugAnnotation::kStringValueFieldNumber,
                             arg.value, end);
      name = arg.name;
      break;
    }
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER: {
      const auto& arg =
          reinterpret_cast<const struct PerfettoTeHlExtraDebugArgPointer&>(
              extra);
      end = WriteVarIntField(DebugAnnotation::kPointerValueFieldNumber,
                             arg.value, end);
      name = arg.name;
      break;
    }
  }

  if (name) {
    auto res = incr->iids.FindOrAssignString(
        protos::pbzero::InternedData::kDebugAnnotationNamesFieldNumber, name);
    if (PERFETTO_UNLIKELY(res.newly_assigned)) {
      auto* ser = incr->serialized_interned_data->add_debug_annotation_names();
      ser->set_iid(res.iid);
      ser->set_name(name);
    }
    end = WriteVarIntField(DebugAnnotation::kNameIidFieldNumber, res.iid, end);
  }
  return EndNestedField(payload, end);
}

// Encodes every field of the TrackEvent of `ev` except its proto fields
// (PERFETTO_TE_PROTO_FIELDS), into `buf`. `buf` must have room for
// `ev.fields_size_bound` bytes. New interned data goes into
// `incr->serialized_interned_data`. Returns the end of the encoded bytes.
uint8_t* EncodeTrackEventFields(const HlEvent& ev,
                                std::optional<uint64_t> track_uuid,
                                TrackEventIncrementalState* incr,
                                uint8_t* buf) {
  using protos::pbzero::TrackEvent;
  const uint8_t* const start = buf;
  if (ev.type != TrackEvent::TYPE_UNSPECIFIED) {
    buf = WriteVarIntField(TrackEvent::kTypeFieldNumber,
                           static_cast<uint64_t>(ev.type), buf);
  }

  const bool has_category = ev.type != TrackEvent::TYPE_SLICE_END &&
                            ev.type != TrackEvent::TYPE_COUNTER;
  if (has_category && !ev.dynamic_cat) {
    const uint64_t iid = ev.cat->cat_iid;
    if (PERFETTO_UNLIKELY(incr->MarkCategorySeen(iid))) {
      auto* ser = incr->serialized_interned_data->add_event_categories();
      ser->set_iid(iid);
      ser->set_name(ev.cat->desc->name);
    }
    buf = WriteVarIntField(TrackEvent::kCategoryIidsFieldNumber, iid, buf);
  }

  if (ev.type != TrackEvent::TYPE_SLICE_END && ev.name) {
    if (ev.no_intern) {
      buf = WriteStringField(TrackEvent::kNameFieldNumber, ev.name, buf);
    } else {
      auto res = incr->iids.FindOrAssignString(
          protos::pbzero::InternedData::kEventNamesFieldNumber, ev.name);
      if (PERFETTO_UNLIKELY(res.newly_assigned)) {
        auto* ser = incr->serialized_interned_data->add_event_names();
        ser->set_iid(res.iid);
        ser->set_name(ev.name);
      }
      buf = WriteVarIntField(TrackEvent::kNameIidFieldNumber, res.iid, buf);
    }
  }

  if (has_category && ev.dynamic_cat) {
    buf = WriteStringField(TrackEvent::kCategoriesFieldNumber,
                           ev.dynamic_cat->name, buf);
  }

  if (track_uuid) {
    buf = WriteVarIntField(TrackEvent::kTrackUuidFieldNumber, *track_uuid, buf);
  }

  // The fields of the extras, in the order of the extras. The other extras
  // (tracks, timestamp, dynamic category, flags and proto fields) are handled
  // elsewhere.
  for (const auto* it = ev.extras; *it != nullptr; it++) {
    const struct PerfettoTeHlExtra& extra = **it;
    switch (extra.type) {
      case PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_INT64:
        // Only counter events carry a counter value.
        if (ev.type == TrackEvent::TYPE_COUNTER) {
          buf = WriteVarIntField(
              TrackEvent::kCounterValueFieldNumber,
              static_cast<uint64_t>(
                  reinterpret_cast<const struct PerfettoTeHlExtraCounterInt64&>(
                      extra)
                      .value),
              buf);
        }
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_DOUBLE:
        if (ev.type == TrackEvent::TYPE_COUNTER) {
          buf = WriteDoubleField(
              TrackEvent::kDoubleCounterValueFieldNumber,
              reinterpret_cast<const struct PerfettoTeHlExtraCounterDouble&>(
                  extra)
                  .value,
              buf);
        }
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER:
        buf = EncodeDebugAnnotation(extra, incr, buf);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_FLOW:
        buf = WriteFixed64Field(
            TrackEvent::kFlowIdsFieldNumber,
            reinterpret_cast<const struct PerfettoTeHlExtraFlow&>(extra).id,
            buf);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_TERMINATING_FLOW:
        buf = WriteFixed64Field(
            TrackEvent::kTerminatingFlowIdsFieldNumber,
            reinterpret_cast<const struct PerfettoTeHlExtraFlow&>(extra).id,
            buf);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID:
        buf = WriteVarIntField(
            TrackEvent::kCorrelationIdFieldNumber,
            reinterpret_cast<const struct PerfettoTeHlExtraCorrelationId&>(
                extra)
                .id,
            buf);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_CORRELATION_ID_STR:
        buf = WriteStringField(
            TrackEvent::kCorrelationIdStrFieldNumber,
            reinterpret_cast<const struct PerfettoTeHlExtraCorrelationIdStr&>(
                extra)
                .str,
            buf);
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(static_cast<size_t>(buf - start) <= ev.fields_size_bound);
  return buf;
}

// Emits (once per sequence) the track descriptor for the named track described
// by `track` (its `header` is ignored) and returns the derived track uuid.
uint64_t EmitNamedTrack(uint64_t parent_uuid,
                        const PerfettoTeHlNestedTrackNamed& track,
                        perfetto::shlib::TrackEventIncrementalState* incr_state,
                        perfetto::TraceWriterBase* trace_writer) {
  uint64_t uuid = parent_uuid;
  uuid ^= PerfettoFnv1a(track.name, strlen(track.name));
  uuid ^= track.id;
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

// Emits the track descriptors of a nested track hierarchy, if not emitted yet,
// and returns the uuid of the innermost track.
uint64_t EmitNestedTracks(const PerfettoTeHlExtraNestedTracks& nested,
                          TrackEventIncrementalState* incr_state,
                          perfetto::TraceWriterBase* trace_writer) {
  uint64_t uuid = 0;
  for (PerfettoTeHlNestedTrack* const* tp = nested.tracks; *tp != nullptr;
       tp++) {
    auto track_type =
        static_cast<enum PerfettoTeHlNestedTrackType>((*tp)->type);

    switch (track_type) {
      case PERFETTO_TE_HL_NESTED_TRACK_TYPE_NAMED: {
        auto* named_track =
            reinterpret_cast<PerfettoTeHlNestedTrackNamed*>(*tp);
        uuid = EmitNamedTrack(uuid, *named_track, incr_state, trace_writer);
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
  return uuid;
}

// Emits the track descriptors of the track of `ev`, if not emitted yet, and
// returns its uuid. Returns nullopt for the default track of the sequence.
std::optional<uint64_t> EmitTrack(const HlEvent& ev,
                                  TrackEventIncrementalState* incr_state,
                                  perfetto::TraceWriterBase* trace_writer) {
  switch (ev.track_kind) {
    case TrackKind::kDefault:
      return std::nullopt;
    case TrackKind::kRegistered:
      return EmitRegisteredTrack(
          static_cast<const PerfettoTeRegisteredTrackImpl*>(ev.track),
          incr_state, trace_writer);
    case TrackKind::kNamed: {
      auto* named_track =
          static_cast<const PerfettoTeHlExtraNamedTrack*>(ev.track);
      PerfettoTeHlNestedTrackNamed named{};
      named.name = named_track->name;
      named.id = named_track->id;
      named.is_name_static = named_track->is_name_static;
      return EmitNamedTrack(named_track->parent_uuid, named, incr_state,
                            trace_writer);
    }
    case TrackKind::kProto: {
      auto* proto_track =
          static_cast<const PerfettoTeHlExtraProtoTrack*>(ev.track);
      return EmitProtoTrack(proto_track->uuid, proto_track->fields, incr_state,
                            trace_writer);
    }
    case TrackKind::kNested:
      return EmitNestedTracks(
          *static_cast<const PerfettoTeHlExtraNestedTracks*>(ev.track),
          incr_state, trace_writer);
  }
  return std::nullopt;
}

// Appends the interned data collected while writing the event.
void AppendInternedData(protos::pbzero::TracePacket* packet,
                        TrackEventIncrementalState* incr_state) {
  auto ranges = incr_state->serialized_interned_data.GetRanges();
  packet->AppendScatteredBytes(
      protos::pbzero::TracePacket::kInternedDataFieldNumber, ranges.data(),
      ranges.size());
  incr_state->serialized_interned_data.Reset();
}

// The fields of most events fit in this many bytes on the stack.
constexpr size_t kMaxStackFieldsSize = 512;

// Writes the whole packet of an event with one AppendRawProtoBytes(): the
// packet prologue, then the TrackEvent as a length-delimited field.
// Only for events without proto fields whose fields fit in
// kMaxStackFieldsSize.
void WriteEventPacketDirect(const HlEvent& ev,
                            std::optional<uint64_t> track_uuid,
                            perfetto::TraceWriterBase* trace_writer,
                            TrackEventIncrementalState* incr_state,
                            const TrackEventTlsState& tls_state) {
  // The TrackEvent tag takes one byte, and its length up to two.
  static_assert(kMaxStackFieldsSize < 16384, "length must fit in two bytes");
  PERFETTO_DCHECK(!ev.has_proto_fields &&
                  ev.fields_size_bound <= kMaxStackFieldsSize);
  uint8_t buf[kMaxPacketPrologueSize + 3 + kMaxStackFieldsSize];
  uint8_t* end = EncodePacketPrologue(
      buf, incr_state, tls_state, ev.ts,
      protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE);
  uint8_t* fields = BeginNestedField(
      protos::pbzero::TracePacket::kTrackEventFieldNumber, end);
  end = EndNestedField(
      fields, EncodeTrackEventFields(ev, track_uuid, incr_state, fields));

  auto packet = trace_writer->NewTracePacket();
  packet->AppendRawProtoBytes(buf, static_cast<size_t>(end - buf));
  if (PERFETTO_UNLIKELY(!incr_state->serialized_interned_data.empty()))
    AppendInternedData(packet.get(), incr_state);
}

// Writes the packet of an event through protozero. Events with proto fields
// take this path, because AppendHlProtoFields() uses protozero, and so do
// events whose fields do not fit on the stack.
void WriteEventPacketWithProtozero(const HlEvent& ev,
                                   std::optional<uint64_t> track_uuid,
                                   perfetto::TraceWriterBase* trace_writer,
                                   TrackEventIncrementalState* incr_state,
                                   const TrackEventTlsState& tls_state) {
  auto packet = NewTracePacketInternal(
      trace_writer, incr_state, tls_state, ev.ts,
      protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE);
  auto* track_event = packet->set_track_event();
  {
    uint8_t stack_buf[kMaxStackFieldsSize];
    std::unique_ptr<uint8_t[]> heap_buf;
    uint8_t* buf = stack_buf;
    if (ev.fields_size_bound > sizeof(stack_buf)) {
      heap_buf.reset(new uint8_t[ev.fields_size_bound]);
      buf = heap_buf.get();
    }
    uint8_t* end = EncodeTrackEventFields(ev, track_uuid, incr_state, buf);
    track_event->AppendRawProtoBytes(buf, static_cast<size_t>(end - buf));
  }

  if (ev.has_proto_fields) {
    for (const auto* it = ev.extras; *it != nullptr; it++) {
      if ((*it)->type == PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS) {
        AppendHlProtoFields(
            incr_state, track_event,
            reinterpret_cast<const struct PerfettoTeHlExtraProtoFields&>(**it)
                .fields);
      }
    }
  }

  if (!incr_state->serialized_interned_data.empty())
    AppendInternedData(packet.get(), incr_state);
}

void InstanceOp(internal::DataSourceType* ds,
                internal::DataSourceType::InstancesIterator* ii,
                internal::DataSourceThreadLocalState* tls_state,
                const HlEvent& ev) {
  if (PERFETTO_UNLIKELY(ev.dynamic_cat)) {
    AdvanceToFirstEnabledDynamicCategory(ii, tls_state, ev.cat,
                                         *ev.dynamic_cat);
    if (!ii->instance) {
      return;
    }
  }

  perfetto::TraceWriterBase* trace_writer = ii->instance->trace_writer.get();

  const auto& track_event_tls = *static_cast<TrackEventTlsState*>(
      ii->instance->data_source_custom_tls.get());

  auto* incr_state = static_cast<TrackEventIncrementalState*>(
      ds->GetIncrementalState(ii->instance, ii->i));
  ResetIncrementalStateIfRequired(trace_writer, incr_state, track_event_tls,
                                  ev.ts);

  std::optional<uint64_t> track_uuid = EmitTrack(ev, incr_state, trace_writer);

  if (PERFETTO_LIKELY(!ev.has_proto_fields &&
                      ev.fields_size_bound <= kMaxStackFieldsSize)) {
    WriteEventPacketDirect(ev, track_uuid, trace_writer, incr_state,
                           track_event_tls);
  } else {
    WriteEventPacketWithProtozero(ev, track_uuid, trace_writer, incr_state,
                                  track_event_tls);
  }

  if (PERFETTO_UNLIKELY(ev.flush)) {
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

  // The event is the same for every data source instance, so it is parsed and
  // timestamped once. Every instance gets the same timestamp.
  const HlEvent ev = ParseEvent(cat, type, name, extra_data);

  for (perfetto::internal::DataSourceType::InstancesIterator ii =
           ds->BeginIteration<perfetto::shlib::TracePointTraits>(
               cached_instances, tls_state, {cat});
       ii.instance;
       ds->NextIteration</*Traits=*/perfetto::shlib::TracePointTraits>(
           &ii, tls_state, {cat})) {
    perfetto::shlib::InstanceOp(ds, &ii, tls_state, ev);
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
