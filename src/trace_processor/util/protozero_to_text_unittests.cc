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

#include "src/trace_processor/util/protozero_to_text.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/track_event/chrome_compositor_scheduler_state.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/importers/proto/track_event.descriptor.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace protozero_to_text {

namespace {

constexpr size_t kChunkSize = 42;

using ::testing::_;
using ::testing::Eq;

TEST(ProtozeroToTextTest, TrackEventBasic) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();
  EXPECT_EQ(
      "track_uuid: 4\ntimestamp_delta_us: 3",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
  EXPECT_EQ(
      "track_uuid: 4 timestamp_delta_us: 3",
      ShortDebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
}

TEST(ProtozeroToTextTest, TrackEventNestedMsg) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  auto* state = msg->set_cc_scheduler_state();
  state->set_deadline_us(7);
  auto* machine = state->set_state_machine();
  auto* minor_state = machine->set_minor_state();
  minor_state->set_commit_count(8);
  state->set_observing_begin_frame_source(true);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();

  EXPECT_EQ(
      R"(track_uuid: 4
cc_scheduler_state: {
  deadline_us: 7
  state_machine: {
    minor_state: {
      commit_count: 8
    }
  }
  observing_begin_frame_source: true
}
timestamp_delta_us: 3)",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));

  EXPECT_EQ(
      "track_uuid: 4 cc_scheduler_state: { deadline_us: 7 state_machine: { "
      "minor_state: { commit_count: 8 } } observing_begin_frame_source: true } "
      "timestamp_delta_us: 3",
      ShortDebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
}

TEST(ProtozeroToTextTest, TrackEventEnumNames) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_type(TrackEvent::TYPE_SLICE_BEGIN);
  auto binary_proto = msg.SerializeAsArray();
  EXPECT_EQ(
      "type: TYPE_SLICE_BEGIN",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
  EXPECT_EQ(
      "type: TYPE_SLICE_BEGIN",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
}

TEST(ProtozeroToTextTest, CustomDescriptorPoolBasic) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  ASSERT_TRUE(status.ok());
  EXPECT_EQ("track_uuid: 4\ntimestamp_delta_us: 3",
            ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                            kIncludeNewLines));
  EXPECT_EQ("track_uuid: 4 timestamp_delta_us: 3",
            ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                            kSkipNewLines));
}

TEST(ProtozeroToTextTest, CustomDescriptorPoolNestedMsg) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  auto* state = msg->set_cc_scheduler_state();
  state->set_deadline_us(7);
  auto* machine = state->set_state_machine();
  auto* minor_state = machine->set_minor_state();
  minor_state->set_commit_count(8);
  state->set_observing_begin_frame_source(true);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  ASSERT_TRUE(status.ok());

  EXPECT_EQ(
      R"(track_uuid: 4
cc_scheduler_state: {
  deadline_us: 7
  state_machine: {
    minor_state: {
      commit_count: 8
    }
  }
  observing_begin_frame_source: true
}
timestamp_delta_us: 3)",
      ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                      kIncludeNewLines));

  EXPECT_EQ(
      "track_uuid: 4 cc_scheduler_state: { deadline_us: 7 state_machine: { "
      "minor_state: { commit_count: 8 } } observing_begin_frame_source: true } "
      "timestamp_delta_us: 3",
      ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                      kSkipNewLines));
}

TEST(ProtozeroToTextTest, EnumToString) {
  using perfetto::protos::pbzero::TrackEvent;
  EXPECT_EQ("TYPE_SLICE_END",
            ProtozeroEnumToText(".perfetto.protos.TrackEvent.Type",
                                TrackEvent::TYPE_SLICE_END));
}

TEST(ProtozeroToTextTest, UnknownField) {
  using perfetto::protos::pbzero::TrackEvent;
  // Wrong type to force unknown field:
  const auto type = ".perfetto.protos.ChromeCompositorSchedulerState";
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  auto* state = msg->set_cc_scheduler_state();
  state->set_deadline_us(7);
  auto* machine = state->set_state_machine();
  auto* minor_state = machine->set_minor_state();
  minor_state->set_commit_count(8);
  auto bytes = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(ProtozeroToText(pool, type, bytes, kIncludeNewLines),
            "# Ignoring unknown field with id: 24");
}

TEST(ProtozeroToTextTest, BytesField) {
  EXPECT_EQ(BytesToHexEncodedStringForTesting("abc"), R"(\x61\x62\x63)");
}

}  // namespace
}  // namespace protozero_to_text
}  // namespace trace_processor
}  // namespace perfetto
