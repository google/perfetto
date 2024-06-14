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

#include "protos/perfetto/common/trace_stats.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {
// The trace packet uid must be less than or equal to 9999 (nobody). If it is
// anything else, the packet is invalid.
int32_t kValid = 1000;
int32_t kLastValid = Context::kMaxTrustedUid;
int32_t kInvalidUid = 12000;

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

  packet.set_trusted_uid(kValid);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InclusiveEnd) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kLastValid);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceBundleHasLostEvents) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_ftrace_events()->set_lost_events(true);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketFtraceBundleHasNoLostEvents) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_ftrace_events()->set_lost_events(false);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceBundleMissingCpu) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_ftrace_events();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceBundleHasErrors) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_ftrace_events()->add_error();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketFtraceBundle) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  // A bundle doesn't need to have anything in it (other than cpu).
  auto* ftrace_events = packet.mutable_ftrace_events();
  ftrace_events->set_cpu(kSomeCpu);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFtraceEventMissingPid) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

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

  packet.set_trusted_uid(kValid);

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

  packet.set_trusted_uid(kValid);

  auto* ftrace_events = packet.mutable_ftrace_events();
  ftrace_events->set_cpu(kSomeCpu);

  auto* event = ftrace_events->add_event();
  event->set_pid(kSomePid);
  event->set_timestamp(kSomeTime);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketProcessTreeMissingTime) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  // When the packet has a process tree, the packet must have a timestamp.
  packet.mutable_process_tree();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketProcessTree) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  // When the packet has a process tree, the packet must have a timestamp.
  packet.mutable_process_tree();
  packet.set_timestamp(kSomeTime);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketProcessStatsMissingTime) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  // When the packet has process stats, the packet must have a timestamp.
  packet.mutable_process_stats();

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketTraceStatsFlushFailed) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->set_flushes_failed(true);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketTraceStatsNoFlushFailed) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->set_flushes_failed(false);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketFinalFlushSucceeded) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->set_final_flush_outcome(
      protos::gen::TraceStats::FINAL_FLUSH_SUCCEEDED);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketFinalFlushUnspecified) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->set_final_flush_outcome(
      protos::gen::TraceStats::FINAL_FLUSH_UNSPECIFIED);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketFinalFlushFailed) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->set_final_flush_outcome(
      protos::gen::TraceStats::FINAL_FLUSH_FAILED);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketBufferStatsPatchesFailed) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->add_buffer_stats()->set_patches_failed(3);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketBufferStatsNoPatchesFailed) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->add_buffer_stats()->set_patches_failed(0);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketBufferStatsAbiViolation) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->add_buffer_stats()->set_abi_violations(3);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketBufferStatsNoAbiViolation) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()->add_buffer_stats()->set_abi_violations(0);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, InvalidPacketBufferStatsTraceWriterPacketLoss) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()
      ->add_buffer_stats()
      ->set_trace_writer_packet_loss(3);

  ASSERT_EQ(packet.trace_stats().buffer_stats_size(), 1);

  ASSERT_FALSE(Verify(packet).ok());
}

TEST_F(VerifyIntegrityUnitTest,
       InvalidPacketBufferStatsNoTraceWriterPacketLoss) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  packet.mutable_trace_stats()
      ->add_buffer_stats()
      ->set_trace_writer_packet_loss(0);

  ASSERT_OK(Verify(packet));
}

TEST_F(VerifyIntegrityUnitTest, ValidPacketProcessStats) {
  protos::gen::TracePacket packet;

  packet.set_trusted_uid(kValid);

  // When the packet has a process tree, the packet must have a timestamp.
  packet.mutable_process_stats();
  packet.set_timestamp(kSomeTime);

  ASSERT_OK(Verify(packet));
}

}  // namespace perfetto::trace_redaction
