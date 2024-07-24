/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/broadphase_packet_filter.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

class BroadphasePacketFilterTest : public testing::Test {
 protected:
  BroadphasePacketFilter transform_;
  Context context_;
  protos::gen::TracePacket builder_;
};

TEST_F(BroadphasePacketFilterTest, ReturnErrorForEmptyMasks) {
  auto buffer = builder_.SerializeAsString();
  ASSERT_FALSE(transform_.Transform(context_, &buffer).ok());
}

TEST_F(BroadphasePacketFilterTest, ReturnErrorForEmptyPacketMask) {
  // Set the ftrace mask to ensure the error is from the packet mask.
  context_.ftrace_mask.set(0);

  auto buffer = builder_.SerializeAsString();
  ASSERT_FALSE(transform_.Transform(context_, &buffer).ok());
}

TEST_F(BroadphasePacketFilterTest, ReturnErrorForEmptyFtraceMask) {
  // Set the ftrace mask to ensure the error is from the ftrace mask.
  context_.packet_mask.set(0);

  auto buffer = builder_.SerializeAsString();
  ASSERT_FALSE(transform_.Transform(context_, &buffer).ok());
}

TEST_F(BroadphasePacketFilterTest, ReturnErrorForNullPacket) {
  // Set the masks to ensure the error is from the ftrace mask.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(0);

  ASSERT_FALSE(transform_.Transform(context_, nullptr).ok());
}

TEST_F(BroadphasePacketFilterTest, ReturnErrorForEmptyPacket) {
  // Set the masks to ensure the error is from the ftrace mask.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(0);

  std::string buffer;
  ASSERT_FALSE(transform_.Transform(context_, &buffer).ok());
}

TEST_F(BroadphasePacketFilterTest, DropsPacketField) {
  constexpr uint64_t kTime = 1000;

  builder_.set_timestamp(kTime);
  auto buffer = builder_.SerializeAsString();

  // Both masks need some bit set.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(0);

  ASSERT_OK(transform_.Transform(context_, &buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(buffer));

  // The timestamp field should have been dropped.
  ASSERT_FALSE(packet.has_timestamp());
}

TEST_F(BroadphasePacketFilterTest, KeepsPacketField) {
  constexpr uint64_t kTime = 1000;

  builder_.set_timestamp(kTime);
  auto buffer = builder_.SerializeAsString();

  // Both masks need some bit set.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(protos::pbzero::TracePacket::kTimestampFieldNumber);

  ASSERT_OK(transform_.Transform(context_, &buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(buffer));

  ASSERT_TRUE(packet.has_timestamp());
  ASSERT_EQ(packet.timestamp(), kTime);
}

TEST_F(BroadphasePacketFilterTest, DropsAllFtraceEvents) {
  constexpr uint64_t kTime = 1000;

  builder_.mutable_ftrace_events()->add_event()->set_timestamp(kTime);
  auto buffer = builder_.SerializeAsString();

  // Both masks need some bit set.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(0);

  ASSERT_OK(transform_.Transform(context_, &buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(buffer));

  // Because kEvents was not set, all ftrace events will be dropped.
  ASSERT_FALSE(packet.has_ftrace_events());
}

TEST_F(BroadphasePacketFilterTest, KeepFtraceEvents) {
  constexpr uint64_t kTime = 1000;
  constexpr int32_t kCpu = 3;

  builder_.mutable_ftrace_events()->add_event()->set_timestamp(kTime);
  builder_.mutable_ftrace_events()->set_cpu(kCpu);

  auto buffer = builder_.SerializeAsString();

  // Both masks need some bit set.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  ASSERT_OK(transform_.Transform(context_, &buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(buffer));

  // The bundle will be kept. Ignoring the events, the other fields should be
  // copied over. To be simple, we're only checking one field (CPU).
  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_TRUE(packet.ftrace_events().has_cpu());
}

TEST_F(BroadphasePacketFilterTest, KeepsFtraceEvent) {
  constexpr uint64_t kTime = 1000;

  auto* event = builder_.mutable_ftrace_events()->add_event();
  event->set_timestamp(kTime);
  event->mutable_print()->set_buf("hello world");
  auto buffer = builder_.SerializeAsString();

  // Both masks need some bit set.
  context_.ftrace_mask.set(protos::pbzero::FtraceEvent::kPrintFieldNumber);
  context_.packet_mask.set(
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  ASSERT_OK(transform_.Transform(context_, &buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(buffer));

  // kFtraceEvents must be in the packet mask in order for the ftrace events to
  // be searched.
  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);
  ASSERT_TRUE(packet.ftrace_events().event().at(0).has_print());
}

TEST_F(BroadphasePacketFilterTest, DropsFtraceEvent) {
  constexpr uint64_t kTime = 1000;

  auto* event = builder_.mutable_ftrace_events()->add_event();
  event->set_timestamp(kTime);
  event->mutable_print()->set_buf("hello world");
  auto buffer = builder_.SerializeAsString();

  // Both masks need some bit set.
  context_.ftrace_mask.set(0);
  context_.packet_mask.set(
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  ASSERT_OK(transform_.Transform(context_, &buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(buffer));

  // The ftrace events bundle will be copied. All the ftrace events will be
  // copied, but the tasks in the events (e.g. print) will be removed.
  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);
  ASSERT_FALSE(packet.ftrace_events().event().at(0).has_print());
}

}  // namespace perfetto::trace_redaction
