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

#include "src/trace_processor/importers/proto/track_event_record.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/dynamic_string_writer.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/selective_track_event_decoder.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

namespace {

using protos::pbzero::TrackEvent;
using protozero::proto_utils::ProtoWireType;

static_assert(
    std::is_same_v<decltype(TrackEventRecordHeader::flow_count),
                   decltype(TrackEventRecordHeader::terminating_flow_count)>);
// Events with more flows of either kind than this take the generic path.
constexpr size_t kMaxFlows =
    std::numeric_limits<decltype(TrackEventRecordHeader::flow_count)>::max();

}  // namespace

StringPool::Id InternTrackEventCategory(TraceStorage* storage,
                                        PacketSequenceStateGeneration* state,
                                        const uint64_t* iids,
                                        size_t iid_count,
                                        const protozero::ConstChars* strings,
                                        size_t string_count) {
  if (iid_count + string_count == 0) {
    return kNullStringId;
  }
  if (iid_count == 1 && string_count == 0) {
    if (auto id = state->InternedStringId(
            protos::pbzero::InternedData::kEventCategoriesFieldNumber,
            iids[0])) {
      return *id;
    }
    base::DynamicStringWriter writer;
    writer.AppendLiteral("unknown(");
    writer.AppendUnsignedInt(iids[0]);
    writer.AppendChar(')');
    return storage->InternString(writer.GetStringView());
  }
  if (iid_count == 0 && string_count == 1) {
    return storage->InternString(strings[0]);
  }
  // There is a single "cat" column, so several categories are joined.
  std::string categories;
  for (size_t i = 0; i < iid_count; ++i) {
    auto name = state->InternedStringView(
        protos::pbzero::InternedData::kEventCategoriesFieldNumber, iids[i]);
    if (!name) {
      continue;
    }
    if (!categories.empty()) {
      categories.append(",");
    }
    categories.append(name->data(), name->size());
  }
  for (size_t i = 0; i < string_count; ++i) {
    if (!categories.empty()) {
      categories.append(",");
    }
    categories.append(strings[i].data, strings[i].size);
  }
  if (categories.empty()) {
    return kNullStringId;
  }
  return storage->InternString(base::StringView(categories));
}

StringPool::Id InternTrackEventName(TraceStorage* storage,
                                    PacketSequenceStateGeneration* state,
                                    uint64_t name_iid,
                                    std::optional<protozero::ConstChars> name) {
  if (name_iid) {
    if (auto id = state->InternedStringId(
            protos::pbzero::InternedData::kEventNamesFieldNumber, name_iid)) {
      return *id;
    }
    return kNullStringId;
  }
  if (name) {
    return storage->InternString(*name);
  }
  return kNullStringId;
}

size_t BuildTrackEventRecord(protozero::ConstBytes event,
                             TraceStorage* storage,
                             PacketSequenceStateGeneration* state,
                             uint8_t* out,
                             size_t capacity) {
  PERFETTO_DCHECK(capacity >= MaxTrackEventRecordSize(event.size));
  TrackEventRecordHeader header{};
  bool has_type = false;
  uint64_t name_iid = 0;
  std::optional<protozero::ConstChars> name;
  base::SmallVector<uint64_t, 4> category_iids;
  base::SmallVector<protozero::ConstChars, 4> categories;
  // Old and new flow ids are never mixed: the new ones win if present.
  base::SmallVector<uint64_t, 4> flows[2];
  base::SmallVector<uint64_t, 4> old_flows[2];

  // Arg fields are copied as they are met, straight after the header: they
  // are at most the event's size.
  uint8_t* ptr = out + sizeof(header);
  protozero::ProtoDecoder decoder(event);
  for (;;) {
    size_t start = decoder.read_offset();
    protozero::Field field = decoder.ReadField();
    if (!field.valid()) {
      break;
    }
    // Repeated fields are expected unpacked, as producers write them.
    bool is_varint = field.type() == ProtoWireType::kVarInt;
    bool is_fixed64 = field.type() == ProtoWireType::kFixed64;
    bool is_bytes = field.type() == ProtoWireType::kLengthDelimited;
    switch (field.id()) {
      case TrackEvent::kTypeFieldNumber:
        // Counters and states need processing at tokenization.
        if (!is_varint || field.as_int32() == TrackEvent::TYPE_COUNTER ||
            field.as_int32() == TrackEvent::TYPE_STATE ||
            field.as_uint64() > UINT8_MAX) {
          return 0;
        }
        header.type = static_cast<uint8_t>(field.as_uint32());
        has_type = true;
        continue;
      case TrackEvent::kTrackUuidFieldNumber:
        if (!is_varint) {
          return 0;
        }
        header.track_uuid = field.as_uint64();
        header.flags |= TrackEventRecordHeader::kHasTrackUuid;
        continue;
      case TrackEvent::kNameIidFieldNumber:
        if (!is_varint) {
          return 0;
        }
        name_iid = field.as_uint64();
        continue;
      case TrackEvent::kNameFieldNumber:
        if (!is_bytes) {
          return 0;
        }
        name = field.as_string();
        continue;
      case TrackEvent::kCategoryIidsFieldNumber:
        if (!is_varint) {
          return 0;
        }
        category_iids.emplace_back(field.as_uint64());
        continue;
      case TrackEvent::kCategoriesFieldNumber:
        if (!is_bytes) {
          return 0;
        }
        categories.emplace_back(field.as_string());
        continue;
      case TrackEvent::kFlowIdsFieldNumber:
      case TrackEvent::kTerminatingFlowIdsFieldNumber: {
        auto& list =
            flows[field.id() == TrackEvent::kFlowIdsFieldNumber ? 0 : 1];
        if (!is_fixed64 || list.size() == kMaxFlows) {
          return 0;
        }
        list.emplace_back(field.as_uint64());
        continue;
      }
      case TrackEvent::kFlowIdsOldFieldNumber:
      case TrackEvent::kTerminatingFlowIdsOldFieldNumber: {
        auto& list =
            old_flows[field.id() == TrackEvent::kFlowIdsOldFieldNumber ? 0 : 1];
        if (!is_varint || list.size() == kMaxFlows) {
          return 0;
        }
        list.emplace_back(field.as_uint64());
        continue;
      }
      default:
        break;
    }
    if (internal::kTrackEventCoreMask.contains(field.id())) {
      return 0;
    }
    // An arg field: kept as it was on the wire.
    size_t size = decoder.read_offset() - start;
    memcpy(ptr, event.data + start, size);
    ptr += size;
  }
  // A malformed event, or one without a type, goes through the generic
  // path, which reports it.
  if (decoder.bytes_left() != 0 || !has_type) {
    return 0;
  }

  const auto& begin_flows = flows[0].empty() ? old_flows[0] : flows[0];
  const auto& end_flows = flows[1].empty() ? old_flows[1] : flows[1];
  size_t flow_bytes =
      (begin_flows.size() + end_flows.size()) * sizeof(uint64_t);
  // Old flow ids can take more space here than on the wire.
  if (ptr + flow_bytes + sizeof(uint32_t) > out + capacity) {
    return 0;
  }
  header.flow_count = static_cast<uint8_t>(begin_flows.size());
  header.terminating_flow_count = static_cast<uint8_t>(end_flows.size());
  memcpy(ptr, begin_flows.data(), begin_flows.size() * sizeof(uint64_t));
  ptr += begin_flows.size() * sizeof(uint64_t);
  memcpy(ptr, end_flows.data(), end_flows.size() * sizeof(uint64_t));
  ptr += end_flows.size() * sizeof(uint64_t);

  header.name_id =
      InternTrackEventName(storage, state, name_iid, name).raw_id();
  StringPool::Id category = InternTrackEventCategory(
      storage, state, category_iids.data(), category_iids.size(),
      categories.data(), categories.size());
  if (!category.is_null()) {
    header.flags |= TrackEventRecordHeader::kHasCategory;
    uint32_t raw = category.raw_id();
    memcpy(ptr, &raw, sizeof(raw));
    ptr += sizeof(raw);
  }
  memcpy(out, &header, sizeof(header));
  return static_cast<size_t>(ptr - out);
}

}  // namespace perfetto::trace_processor
