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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_RECORD_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_RECORD_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor {

class PacketSequenceStateGeneration;
class TraceStorage;

// The category of an event: its interned (|iids|) and inline (|strings|)
// categories, joined with "," if there is more than one. A lone interned
// category missing from |state| is "unknown(<iid>)"; with several, missing
// ones are skipped.
StringPool::Id InternTrackEventCategory(TraceStorage* storage,
                                        PacketSequenceStateGeneration* state,
                                        const uint64_t* iids,
                                        size_t iid_count,
                                        const protozero::ConstChars* strings,
                                        size_t string_count);

// The name of an event: its interned name if |name_iid| is set, else |name|.
StringPool::Id InternTrackEventName(TraceStorage* storage,
                                    PacketSequenceStateGeneration* state,
                                    uint64_t name_iid,
                                    std::optional<protozero::ConstChars> name);

// A TrackEvent decoded once, at tokenization, into what the importer needs:
// the event's own fields with its name and category already resolved, then
// its arg fields in their wire encoding and wire order. Only events whose own
// fields are all held here become records; every other event keeps its
// TrackEvent bytes.
//
// Layout: the header, then the arg fields, then |flow_count| and
// |terminating_flow_count| u64 flow ids, then the category's StringId if
// kHasCategory is set. The arg fields come first so that a record can be
// written in one forward pass over the event.
struct TrackEventRecordHeader {
  uint64_t track_uuid;
  uint32_t name_id;
  uint8_t type;
  uint8_t flags;
  uint8_t flow_count;
  uint8_t terminating_flow_count;

  static constexpr uint8_t kHasTrackUuid = 1 << 0;
  static constexpr uint8_t kHasCategory = 1 << 1;
};
static_assert(sizeof(TrackEventRecordHeader) == 16);

// The space to give BuildTrackEventRecord for an event of |event_size| bytes.
constexpr size_t MaxTrackEventRecordSize(size_t event_size) {
  return sizeof(TrackEventRecordHeader) + sizeof(uint32_t) + event_size;
}

// Encodes |event| as a record into the |capacity| bytes at |out|, resolving
// its name and category against |state|, and returns the record's size.
// Returns 0, leaving |out| in an unspecified state, if the event has a field
// of its own which records do not hold, has no type, is malformed or if the
// record does not fit.
size_t BuildTrackEventRecord(protozero::ConstBytes event,
                             TraceStorage* storage,
                             PacketSequenceStateGeneration* state,
                             uint8_t* out,
                             size_t capacity);

// Reads a record with the accessors of the TrackEvent decoder, so the importer
// can be instantiated on either. Fields a record never holds read as absent,
// through has_*() accessors which are constexpr false.
class TrackEventRecordView {
 public:
  // The arg fields, in wire order.
  class FieldRange {
   public:
    // Iteration ends at the first field which is not valid, so end() is a
    // sentinel.
    struct End {};
    class Iterator {
     public:
      Iterator(const uint8_t* begin, const uint8_t* end)
          : decoder_(begin, static_cast<size_t>(end - begin)),
            field_(decoder_.ReadField()) {}
      const protozero::Field& operator*() const { return field_; }
      Iterator& operator++() {
        field_ = decoder_.ReadField();
        return *this;
      }
      bool operator!=(End) const { return field_.valid(); }

     private:
      protozero::ProtoDecoder decoder_;
      protozero::Field field_;
    };
    FieldRange(const uint8_t* begin, const uint8_t* end)
        : begin_(begin), end_(end) {}
    Iterator begin() const { return Iterator(begin_, end_); }
    End end() const { return End(); }

   private:
    const uint8_t* begin_;
    const uint8_t* end_;
  };

  // Flow ids, iterated like a repeated field.
  class FlowIds {
   public:
    FlowIds(const uint8_t* ptr, size_t count) : ptr_(ptr), left_(count) {}
    explicit operator bool() const { return left_ > 0; }
    uint64_t operator*() const {
      uint64_t id;
      memcpy(&id, ptr_, sizeof(id));
      return id;
    }
    FlowIds& operator++() {
      ptr_ += sizeof(uint64_t);
      left_--;
      return *this;
    }

   private:
    const uint8_t* ptr_;
    size_t left_;
  };

  explicit TrackEventRecordView(protozero::ConstBytes record) {
    PERFETTO_DCHECK(record.size >= sizeof(TrackEventRecordHeader));
    memcpy(&header_, record.data, sizeof(header_));
    args_begin_ = record.data + sizeof(header_);
    const uint8_t* end = record.data + record.size;
    if (header_.flags & TrackEventRecordHeader::kHasCategory) {
      PERFETTO_DCHECK(end - args_begin_ >=
                      static_cast<ptrdiff_t>(sizeof(category_id_)));
      end -= sizeof(category_id_);
      memcpy(&category_id_, end, sizeof(category_id_));
    }
    size_t flow_bytes =
        (size_t{header_.flow_count} + header_.terminating_flow_count) *
        sizeof(uint64_t);
    PERFETTO_DCHECK(static_cast<size_t>(end - args_begin_) >= flow_bytes);
    flows_ = end - flow_bytes;
  }

  StringPool::Id name_id() const {
    return StringPool::Id::Raw(header_.name_id);
  }
  StringPool::Id category_id() const {
    return StringPool::Id::Raw(category_id_);
  }

  // Events without a type never become records.
  static constexpr bool has_type() { return true; }
  int32_t type() const { return header_.type; }
  bool has_track_uuid() const {
    return header_.flags & TrackEventRecordHeader::kHasTrackUuid;
  }
  uint64_t track_uuid() const { return header_.track_uuid; }
  bool has_flow_ids() const { return header_.flow_count > 0; }
  FlowIds flow_ids() const { return FlowIds(flows_, header_.flow_count); }
  bool has_terminating_flow_ids() const {
    return header_.terminating_flow_count > 0;
  }
  FlowIds terminating_flow_ids() const {
    return FlowIds(flows_ + header_.flow_count * sizeof(uint64_t),
                   header_.terminating_flow_count);
  }
  FieldRange unknown_fields() const { return FieldRange(args_begin_, flows_); }

#define PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(name)                            \
  static constexpr bool has_##name() { return false; }                        \
  static decltype(std::declval<protos::pbzero::TrackEvent::Decoder>().name()) \
  name() {                                                                    \
    return {};                                                                \
  }
  // name_iid, name, categories and category_iids are resolved into name_id()
  // and category_id(); flow_ids_old and terminating_flow_ids_old are stored as
  // flow_ids and terminating_flow_ids.
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(name_iid)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(name)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(categories)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(category_iids)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(flow_ids_old)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(terminating_flow_ids_old)
  // Never held by records.
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(callstack)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(callstack_iid)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(callstack_weight)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(correlation_id)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(correlation_id_str)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(correlation_id_str_iid)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(counter_value)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(debug_annotations)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(double_counter_value)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(extra_counter_track_uuids)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(extra_counter_values)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(extra_double_counter_track_uuids)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(extra_double_counter_values)
  PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD(legacy_event)
#undef PERFETTO_TE_RECORD_VIEW_ABSENT_FIELD

 private:
  TrackEventRecordHeader header_;
  uint32_t category_id_ = 0;
  const uint8_t* args_begin_;
  const uint8_t* flows_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_RECORD_H_
