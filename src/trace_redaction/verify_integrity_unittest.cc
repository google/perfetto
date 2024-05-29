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

#include "src/trace_redaction/verify_integrity.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {
// The trace packet uid must be 1000 (system) or 9999 (nobody). If it is
// anything else, the packet is invalid.
int32_t kNobodyUid = 9999;
int32_t kSystemUid = 1000;
int32_t kInvalidUid = 9;

uint64_t kSomeTime = 1234;
uint32_t kSomePid = 7;

uint32_t kSomeCpu = 3;
}  // namespace

class VerifyIntegrityUnitTest : public testing::Test {
 protected:
  base::Status Verify(const protos::gen::TracePacket& packet) {
    auto packet_buffer = packet.SerializeAsString();
    protos::pbzero::TracePacket::Decoder packer_decoder(packet_buffer);

    VerifyIntegrity verify;
    Context context;
    return verify.Collect(packer_decoder, &context);
  }
};

TEST_F(VerifyIntegrityUnitTest, InvalidPacketNoUid) {
  protos::gen::TracePacket packet;
  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketInvalidUid) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kInvalidUid);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketSystemUid) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketNobodyUid) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kNobodyUid);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceBundleMissingCpu) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  packet.mutable_ftrace_events();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketFtraceBundle) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  // A bundle doesn't need to have anything in it (other than cpu).
  auto* ftrace_events = packet.mutable_ftrace_events();
  ftrace_events->set_cpu(kSomeCpu);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceEventMissingPid) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  auto* ftrace_events = packet.mutable_ftrace_events();
  ftrace_events->set_cpu(kSomeCpu);

  // A valid event has a pid and timestamp. Add the time (but not the pid) to
  // ensure the pid caused the error.
  auto* event = ftrace_events->add_event();
  event->set_timestamp(kSomeTime);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceEventMissingTime) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  auto* ftrace_events = packet.mutable_ftrace_events();
  ftrace_events->set_cpu(kSomeCpu);

  // A valid event has a pid and timestamp. Add the pid (but not the time) to
  // ensure the time caused the error.
  auto* event = ftrace_events->add_event();
  event->set_pid(kSomePid);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketFtraceEvent) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  auto* ftrace_events = packet.mutable_ftrace_events();
  ftrace_events->set_cpu(kSomeCpu);

  auto* event = ftrace_events->add_event();
  event->set_pid(kSomePid);
  event->set_timestamp(kSomeTime);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketProcessTreeMissingTime) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  // When the packet has a process tree, the packet must have a timestamp.
  packet.mutable_process_tree();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketProcessTree) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  // When the packet has a process tree, the packet must have a timestamp.
  packet.mutable_process_tree();
  packet.set_timestamp(kSomeTime);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketProcessStatsMissingTime) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  // When the packet has process stats, the packet must have a timestamp.
  packet.mutable_process_stats();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketProcessStats) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kSystemUid);

  // When the packet has a process tree, the packet must have a timestamp.
  packet.mutable_process_stats();
  packet.set_timestamp(kSomeTime);

  ASSERT_OK(Verify(packet));
}

}  // namespace perfetto::trace_redaction
