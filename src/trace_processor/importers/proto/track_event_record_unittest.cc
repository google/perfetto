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
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/protozero/field.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_builder.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using protos::pbzero::InternedData;
using protos::pbzero::TrackEvent;
using ::testing::ElementsAre;

// Field ids in the extension range, which the importer handles as args.
constexpr uint32_t kArgFieldA = 1000;
constexpr uint32_t kArgFieldB = 1001;

class TrackEventRecordTest : public ::testing::Test {
 public:
  TrackEventRecordTest() {
    context_.storage.reset(new TraceStorage());
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.machine_tracker.reset(
        new MachineTracker(&context_, kDefaultMachineId));
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    builder_.emplace(&context_, /*trusted_packet_sequence_id=*/1);
  }

 protected:
  using Event = protozero::HeapBuffered<protozero::Message>;

  // Interns |name| under |iid| in the event names or categories.
  void Intern(uint32_t field_id, uint64_t iid, const std::string& name) {
    protozero::HeapBuffered<protos::pbzero::EventName> msg;
    msg->set_iid(iid);
    msg->set_name(name);
    std::vector<uint8_t> bytes = msg.SerializeAsArray();
    builder_->InternMessage(field_id, TraceBlobView(TraceBlob::CopyFrom(
                                          bytes.data(), bytes.size())));
  }

  PacketSequenceStateGeneration* state() {
    return builder_->current_generation().get();
  }

  StringPool::Id Str(const char* s) {
    return context_.storage->InternString(base::StringView(s));
  }

  // Builds a record of |event|, less its last |truncate| bytes, into
  // |record_|; returns its size.
  size_t Build(Event& event, size_t truncate = 0) {
    event_bytes_ = event.SerializeAsArray();
    event_bytes_.resize(event_bytes_.size() - truncate);
    size_t capacity = MaxTrackEventRecordSize(event_bytes_.size());
    record_.assign(capacity, 0);
    return BuildTrackEventRecord(
        protozero::ConstBytes{event_bytes_.data(), event_bytes_.size()},
        context_.storage.get(), state(), record_.data(), capacity);
  }

  // Builds a record of |event| which must not fall back, and views it.
  TrackEventRecordView BuildView(Event& event) {
    size_t size = Build(event);
    EXPECT_NE(size, 0u);
    return TrackEventRecordView(protozero::ConstBytes{record_.data(), size});
  }

  // The field's value, with fixed64 values prefixed by "f".
  static std::string Describe(const protozero::Field& field) {
    switch (field.type()) {
      case protozero::proto_utils::ProtoWireType::kVarInt:
        return std::to_string(field.as_uint64());
      case protozero::proto_utils::ProtoWireType::kFixed64:
        return "f" + std::to_string(field.as_uint64());
      default:
        return field.as_std_string();
    }
  }

  static std::vector<uint64_t> Flows(TrackEventRecordView::FlowIds it) {
    std::vector<uint64_t> ids;
    for (; it; ++it) {
      ids.push_back(*it);
    }
    return ids;
  }

  TraceProcessorContext context_;
  std::optional<PacketSequenceStateBuilder> builder_;
  std::vector<uint8_t> event_bytes_;
  std::vector<uint8_t> record_;
};

TEST_F(TrackEventRecordTest, RoundTrip) {
  Intern(InternedData::kEventNamesFieldNumber, 1, "slice");
  Intern(InternedData::kEventCategoriesFieldNumber, 1, "a");
  Intern(InternedData::kEventCategoriesFieldNumber, 2, "b");
  Event event;
  event->AppendVarInt(kArgFieldB, 5);
  event->AppendVarInt(TrackEvent::kTypeFieldNumber,
                      TrackEvent::TYPE_SLICE_BEGIN);
  event->AppendVarInt(TrackEvent::kTrackUuidFieldNumber, 1234);
  event->AppendVarInt(TrackEvent::kNameIidFieldNumber, 1);
  event->AppendString(kArgFieldA, "first");
  event->AppendString(TrackEvent::kCategoriesFieldNumber, "c");
  event->AppendVarInt(TrackEvent::kCategoryIidsFieldNumber, 1);
  // Missing from the interned data, so skipped.
  event->AppendVarInt(TrackEvent::kCategoryIidsFieldNumber, 9);
  event->AppendVarInt(TrackEvent::kCategoryIidsFieldNumber, 2);
  event->AppendFixed(kArgFieldB, uint64_t{9});
  // Old flow ids are stored as new ones; per kind, new ones win.
  event->AppendVarInt(TrackEvent::kFlowIdsOldFieldNumber, 4);
  event->AppendFixed(TrackEvent::kFlowIdsFieldNumber, uint64_t{1});
  event->AppendFixed(TrackEvent::kFlowIdsFieldNumber, uint64_t{2});
  event->AppendVarInt(TrackEvent::kTerminatingFlowIdsOldFieldNumber, 5);

  TrackEventRecordView view = BuildView(event);
  EXPECT_TRUE(view.has_type());
  EXPECT_EQ(view.type(), TrackEvent::TYPE_SLICE_BEGIN);
  EXPECT_TRUE(view.has_track_uuid());
  EXPECT_EQ(view.track_uuid(), 1234u);
  EXPECT_EQ(view.name_id(), Str("slice"));
  EXPECT_EQ(view.category_id(), Str("a,b,c"));
  EXPECT_THAT(Flows(view.flow_ids()), ElementsAre(1u, 2u));
  EXPECT_THAT(Flows(view.terminating_flow_ids()), ElementsAre(5u));
  // Arg fields are kept in wire order.
  std::vector<std::pair<uint32_t, std::string>> fields;
  for (const protozero::Field& field : view.unknown_fields()) {
    fields.emplace_back(field.id(), Describe(field));
  }
  EXPECT_THAT(fields,
              ElementsAre(std::make_pair(kArgFieldB, std::string("5")),
                          std::make_pair(kArgFieldA, std::string("first")),
                          std::make_pair(kArgFieldB, std::string("f9"))));

  // An inline name, no track and a single unknown category.
  Event minimal;
  minimal->AppendVarInt(TrackEvent::kTypeFieldNumber, TrackEvent::TYPE_INSTANT);
  minimal->AppendString(TrackEvent::kNameFieldNumber, "inline");
  minimal->AppendVarInt(TrackEvent::kCategoryIidsFieldNumber, 42);
  view = BuildView(minimal);
  EXPECT_EQ(view.type(), TrackEvent::TYPE_INSTANT);
  EXPECT_FALSE(view.has_track_uuid());
  EXPECT_EQ(view.name_id(), Str("inline"));
  EXPECT_EQ(view.category_id(), Str("unknown(42)"));
  EXPECT_FALSE(view.has_flow_ids());
  EXPECT_FALSE(view.has_terminating_flow_ids());
  EXPECT_FALSE(view.unknown_fields().begin() != view.unknown_fields().end());
}

TEST_F(TrackEventRecordTest, FallsBack) {
  struct Case {
    std::string desc;
    std::function<void(Event&)> fill;
    // Bytes dropped from the end of the event.
    size_t truncate = 0;
  };
  auto instant = [](Event& e) {
    e->AppendVarInt(TrackEvent::kTypeFieldNumber, TrackEvent::TYPE_INSTANT);
  };
  std::vector<Case> cases = {
      {"no type",
       [](Event& e) { e->AppendString(TrackEvent::kNameFieldNumber, "n"); }},
      {"counter",
       [](Event& e) {
         e->AppendVarInt(TrackEvent::kTypeFieldNumber,
                         TrackEvent::TYPE_COUNTER);
       }},
      {"state",
       [](Event& e) {
         e->AppendVarInt(TrackEvent::kTypeFieldNumber, TrackEvent::TYPE_STATE);
       }},
      {"type as bytes",
       [](Event& e) {
         e->AppendString(TrackEvent::kTypeFieldNumber, "12345678");
       }},
      {"type out of range",
       [](Event& e) { e->AppendVarInt(TrackEvent::kTypeFieldNumber, 256); }},
      {"too many flows",
       [&](Event& e) {
         instant(e);
         for (uint64_t i = 0; i < 256; ++i) {
           e->AppendFixed(TrackEvent::kTerminatingFlowIdsFieldNumber, i);
         }
       }},
      {"old flow ids outgrow the record",
       [&](Event& e) {
         // Each old flow id takes 2 bytes on the wire but 8 in the record.
         instant(e);
         for (uint64_t i = 1; i <= 4; ++i) {
           e->AppendVarInt(TrackEvent::kFlowIdsOldFieldNumber, i);
         }
       }},
      {"malformed",
       [&](Event& e) {
         instant(e);
         e->AppendString(kArgFieldA, "truncated");
       },
       /*truncate=*/1},
  };
  // Fields a record does not hold.
  for (uint32_t field_id : std::initializer_list<uint32_t>{
           TrackEvent::kDebugAnnotationsFieldNumber,
           TrackEvent::kLegacyEventFieldNumber,
           TrackEvent::kTimestampDeltaUsFieldNumber,
           TrackEvent::kCounterValueFieldNumber,
           TrackEvent::kExtraCounterValuesFieldNumber,
           TrackEvent::kCorrelationIdFieldNumber,
           TrackEvent::kCallstackIidFieldNumber}) {
    cases.push_back({"field " + std::to_string(field_id), [=](Event& e) {
                       instant(e);
                       e->AppendVarInt(field_id, 1);
                     }});
  }
  // Fields in an encoding they are not expected in.
  for (uint32_t field_id : std::initializer_list<uint32_t>{
           TrackEvent::kTrackUuidFieldNumber, TrackEvent::kNameIidFieldNumber,
           TrackEvent::kCategoryIidsFieldNumber,
           TrackEvent::kFlowIdsOldFieldNumber,
           TrackEvent::kTerminatingFlowIdsOldFieldNumber}) {
    cases.push_back({"bytes field " + std::to_string(field_id), [=](Event& e) {
                       instant(e);
                       e->AppendString(field_id, "12345678");
                     }});
  }
  for (uint32_t field_id : std::initializer_list<uint32_t>{
           TrackEvent::kNameFieldNumber, TrackEvent::kCategoriesFieldNumber,
           TrackEvent::kFlowIdsFieldNumber,
           TrackEvent::kTerminatingFlowIdsFieldNumber}) {
    cases.push_back({"varint field " + std::to_string(field_id), [=](Event& e) {
                       instant(e);
                       e->AppendVarInt(field_id, 1);
                     }});
  }

  for (const Case& c : cases) {
    Event event;
    c.fill(event);
    EXPECT_EQ(Build(event, c.truncate), 0u) << c.desc;
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
