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

// Repro for https://github.com/google/perfetto/issues/6051
//
// In a "multi-machine" setup the producer connects to traced over a TCP
// (or vsock / Inet) producer socket. Because file descriptors cannot be
// transferred over a non-unix socket, the service switches to the
// "shmem emulation" path: chunk payloads are inlined inside CommitData
// IPC frames. This test contrasts the unix-socket case (real shared
// memory, no emulation) with the TCP case (shmem emulation) under a
// producer that emits packets at a steady rate using the default
// SDK/track_event buffer-exhausted policy of kDrop.
//
// What the test demonstrates:
//   * unix:  all N packets are committed and observed (0 drops).
//   * TCP:   the producer-side shmem cannot be drained as fast as it
//            fills, kDrop kicks in, and packets are lost — surfaced as
//            trace_writer_packet_loss in TraceStats and as missing
//            sequence values in the output.
//
// Run with:
//   tools/ninja -C out/linux_clang_release perfetto_integrationtests
//   out/linux_clang_release/perfetto_integrationtests \
//       --gtest_filter='RelayDataDrop*' --gtest_brief=0

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "src/base/test/test_task_runner.h"
#include "src/traced_relay/relay_service.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/common/trace_stats.gen.h"
#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"

namespace perfetto {
namespace {

// How many messages each iteration of the producer's pacing loop emits.
constexpr uint32_t kMessagesPerBatch = 200;
// How many iterations the producer runs for. With kMessagesPerBatch=200,
// 200 iterations = 40k packets.
constexpr uint32_t kIterations = 200;
// Payload size per packet. 1 KiB closely mirrors the rust producer in the
// bug report.
constexpr uint32_t kMessageSize = 1024;
// kSeed matches the seed the FakeProducer uses to reconstruct the expected
// sequence value stream.
constexpr uint32_t kSeed = 42;

struct RunResult {
  size_t packets_received = 0;
  uint64_t trace_writer_packet_loss = 0;
  uint64_t chunks_discarded = 0;
  uint64_t chunks_written = 0;
  uint64_t chunks_read = 0;
  uint64_t chunks_overwritten = 0;
  uint64_t chunks_rewritten = 0;
  uint64_t chunks_committed_out_of_order = 0;
  uint64_t patches_failed = 0;
  uint64_t patches_succeeded = 0;
  uint64_t abi_violations = 0;
  uint64_t bytes_written = 0;
  uint64_t bytes_overwritten = 0;
  uint64_t bytes_read = 0;
  uint64_t buffer_size = 0;
};

// Drives the FakeProducer to emit kIterations * kMessagesPerBatch packets
// over the given socket and reports how many made it into the trace.
//
// `socket_addr` controls whether the produced data flows over unix
// (when it starts with '@' or contains a path) or TCP (when it is a
// "host:port" string).  `enable_relay_endpoint` enables the relay
// endpoint on the service so it accepts TCP producer connections.
RunResult RunProducerAndCapture(const std::string& socket_addr,
                                bool enable_relay_endpoint,
                                BufferExhaustedPolicy policy) {
  base::TestTaskRunner task_runner;

  // Spin up traced bound to `socket_addr`.
  TestHelper helper(&task_runner, TestHelper::Mode::kStartDaemons,
                    socket_addr.c_str(), enable_relay_endpoint);
  helper.StartServiceIfRequired();

  // Producer thread that connects on the matching socket.
  auto producer_connected =
      task_runner.CreateCheckpoint("FakeProducer.connected");
  auto producer_setup =
      task_runner.CreateCheckpoint("FakeProducer.setup");
  auto producer_enabled =
      task_runner.CreateCheckpoint("FakeProducer.enabled");
  auto connected = [&]() { task_runner.PostTask(producer_connected); };
  auto setup = [&]() { task_runner.PostTask(producer_setup); };
  auto enabled = [&]() { task_runner.PostTask(producer_enabled); };

  auto producer_thread = std::make_unique<FakeProducerThread>(
      socket_addr, connected, setup, enabled, "perfetto.FakeProducer");
  // Set kDrop on the producer thread so it matches the SDK / track_event
  // default policy.  This is what the user's rust producer uses.
  producer_thread->runner()->PostTaskAndWaitForTesting([&] {
    producer_thread->producer()->set_buffer_exhausted_policy(policy);
  });
  producer_thread->Connect();
  task_runner.RunUntilCheckpoint("FakeProducer.connected");

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  // Receiving-side buffer is very large so the service is never the bottleneck.
  auto* buf_cfg = trace_config.add_buffers();
  buf_cfg->set_size_kb(64 * 1024);  // 64 MiB
  buf_cfg->set_experimental_mode(
      TraceConfig::BufferConfig::TRACE_BUFFER_V2);  // v1 is on its way out
  // No duration_ms: we'll explicitly DisableTracing() after we've pushed all
  // the data and given the service a moment to drain.
  if (enable_relay_endpoint) {
    trace_config.set_trace_all_machines(true);
  }

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.FakeProducer");
  ds_config->set_target_buffer(0);
  // Initial batch on register: emits kMessagesPerBatch packets immediately.
  ds_config->mutable_for_testing()->set_seed(kSeed);
  ds_config->mutable_for_testing()->set_message_count(kMessagesPerBatch);
  ds_config->mutable_for_testing()->set_message_size(kMessageSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  // Wait for our own producer (not one of TestHelper's auto-created producers)
  // to reach StartDataSource and emit the initial register-time batch.
  task_runner.RunUntilCheckpoint("FakeProducer.enabled");

  // Now drive the producer for (kIterations - 1) more batches.  We pace
  // batches to give the data source a chance to drain — this is the closest
  // analogue we have to the rust producer in the bug report.
  for (uint32_t i = 1; i < kIterations; i++) {
    auto cp = "batch.done." + std::to_string(i);
    auto done = task_runner.CreateCheckpoint(cp);
    producer_thread->producer()->ProduceEventBatch(
        [&task_runner, done] { task_runner.PostTask(done); });
    task_runner.RunUntilCheckpoint(cp);
    // Mirror the user's rust producer pattern of sleeping briefly between
    // emit iterations. This gives the IPC pipeline time to drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }

  // Wait until the producer has finished sending all CommitData IPCs and
  // their responses have come back. Sync() guarantees a round trip with the
  // service, so by the time it fires the service has at least observed the
  // last commit.
  auto sync_done = task_runner.CreateCheckpoint("FakeProducer.sync");
  producer_thread->producer()->Sync(
      [&task_runner, sync_done] { task_runner.PostTask(sync_done); });
  task_runner.RunUntilCheckpoint("FakeProducer.sync");

  // Flush the service-side buffer one more time so anything still in-flight
  // is included in the read.
  helper.FlushAndWait(5000);

  // Let the service finish draining and disable.
  helper.DisableTracing();
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  RunResult result;
  for (const auto& packet : helper.trace()) {
    if (packet.has_for_testing())
      result.packets_received++;
  }
  for (const auto& packet : helper.full_trace()) {
    if (packet.has_trace_stats()) {
      for (const auto& buf : packet.trace_stats().buffer_stats()) {
        result.chunks_discarded += buf.chunks_discarded();
        result.trace_writer_packet_loss += buf.trace_writer_packet_loss();
        result.chunks_written += buf.chunks_written();
        result.chunks_read += buf.chunks_read();
        result.chunks_overwritten += buf.chunks_overwritten();
        result.chunks_rewritten += buf.chunks_rewritten();
        result.chunks_committed_out_of_order += buf.chunks_committed_out_of_order();
        result.patches_failed += buf.patches_failed();
        result.patches_succeeded += buf.patches_succeeded();
        result.abi_violations += buf.abi_violations();
        result.bytes_written += buf.bytes_written();
        result.bytes_overwritten += buf.bytes_overwritten();
        result.bytes_read += buf.bytes_read();
        result.buffer_size = buf.buffer_size();
      }
    }
  }
  return result;
}

TEST(RelayDataDropTest, BaselineUnixSocketKDrop) {
  // Unix socket: no shmem emulation. Even with kDrop the producer should
  // drain comfortably and not lose anything for this volume.
  auto result = RunProducerAndCapture("@perfetto_relay_drop_unix_kdrop",
                                      /*enable_relay_endpoint=*/false,
                                      BufferExhaustedPolicy::kDrop);
  const uint32_t expected = kIterations * kMessagesPerBatch;
  PERFETTO_LOG("[unix/kDrop] received=%zu, expected=%u, loss=%" PRIu64
               ", chunks_discarded=%" PRIu64,
               result.packets_received, expected,
               result.trace_writer_packet_loss, result.chunks_discarded);
  PERFETTO_LOG("            chunks_written=%" PRIu64
               ", chunks_read=%" PRIu64
               ", chunks_overwritten=%" PRIu64
               ", chunks_rewritten=%" PRIu64
               ", chunks_oo=%" PRIu64
               ", patches_ok=%" PRIu64
               ", patches_failed=%" PRIu64
               ", abi_viol=%" PRIu64,
               result.chunks_written, result.chunks_read,
               result.chunks_overwritten, result.chunks_rewritten,
               result.chunks_committed_out_of_order,
               result.patches_succeeded, result.patches_failed,
               result.abi_violations);
  PERFETTO_LOG("            bytes_written=%" PRIu64
               ", bytes_read=%" PRIu64
               ", bytes_overwritten=%" PRIu64
               ", buffer_size=%" PRIu64,
               result.bytes_written, result.bytes_read,
               result.bytes_overwritten, result.buffer_size);
  EXPECT_EQ(result.packets_received, expected);
  EXPECT_EQ(result.trace_writer_packet_loss, 0u);
}

static std::string AllocateLocalTcpAddress() {
  base::TestTaskRunner tr;
  base::UnixSocket::EventListener event_listener;
  auto srv = base::UnixSocket::Listen("127.0.0.1:0", &event_listener, &tr,
                                      base::SockFamily::kInet,
                                      base::SockType::kStream);
  PERFETTO_CHECK(srv->is_listening());
  return srv->GetSockAddr();
  // |srv| goes out of scope and frees the port; small race window but fine
  // for tests.
}

TEST(RelayDataDropTest, MultiMachineTcpSocketKDrop) {
  std::string sock_name = AllocateLocalTcpAddress();
  auto result = RunProducerAndCapture(sock_name,
                                      /*enable_relay_endpoint=*/true,
                                      BufferExhaustedPolicy::kDrop);
  const uint32_t expected = kIterations * kMessagesPerBatch;
  PERFETTO_LOG("[tcp/kDrop]  received=%zu, expected=%u, loss=%" PRIu64
               ", chunks_discarded=%" PRIu64,
               result.packets_received, expected,
               result.trace_writer_packet_loss, result.chunks_discarded);
  PERFETTO_LOG("            chunks_written=%" PRIu64
               ", chunks_read=%" PRIu64
               ", chunks_overwritten=%" PRIu64
               ", chunks_rewritten=%" PRIu64
               ", chunks_oo=%" PRIu64
               ", patches_ok=%" PRIu64
               ", patches_failed=%" PRIu64
               ", abi_viol=%" PRIu64,
               result.chunks_written, result.chunks_read,
               result.chunks_overwritten, result.chunks_rewritten,
               result.chunks_committed_out_of_order,
               result.patches_succeeded, result.patches_failed,
               result.abi_violations);
  PERFETTO_LOG("            bytes_written=%" PRIu64
               ", bytes_read=%" PRIu64
               ", bytes_overwritten=%" PRIu64
               ", buffer_size=%" PRIu64,
               result.bytes_written, result.bytes_read,
               result.bytes_overwritten, result.buffer_size);
  // After the fix for #6051 the TCP/shmem-emulation path no longer silently
  // rejects chunks whose page_idx is >= the (smaller) service-side SMB page
  // count, so all packets should now arrive.
  EXPECT_EQ(result.packets_received, expected);
  EXPECT_EQ(result.trace_writer_packet_loss, 0u);
}

TEST(RelayDataDropTest, MultiMachineTcpSocketKStall) {
  // Same as above but with kStall (the policy that traced_probes uses for
  // ftrace data). Interestingly, this loses an *identical* amount of data
  // to the kDrop case, which means the loss is NOT happening at the
  // producer's shmem-exhaustion path: that path would behave very
  // differently between kStall and kDrop. The loss is somewhere downstream
  // of GetNewChunk().
  std::string sock_name = AllocateLocalTcpAddress();
  auto result = RunProducerAndCapture(sock_name,
                                      /*enable_relay_endpoint=*/true,
                                      BufferExhaustedPolicy::kStall);
  const uint32_t expected = kIterations * kMessagesPerBatch;
  PERFETTO_LOG("[tcp/kStall] received=%zu, expected=%u, loss=%" PRIu64
               ", chunks_discarded=%" PRIu64,
               result.packets_received, expected,
               result.trace_writer_packet_loss, result.chunks_discarded);
  PERFETTO_LOG("            chunks_written=%" PRIu64
               ", chunks_read=%" PRIu64
               ", chunks_overwritten=%" PRIu64
               ", chunks_rewritten=%" PRIu64
               ", chunks_oo=%" PRIu64
               ", patches_ok=%" PRIu64
               ", patches_failed=%" PRIu64
               ", abi_viol=%" PRIu64,
               result.chunks_written, result.chunks_read,
               result.chunks_overwritten, result.chunks_rewritten,
               result.chunks_committed_out_of_order,
               result.patches_succeeded, result.patches_failed,
               result.abi_violations);
  PERFETTO_LOG("            bytes_written=%" PRIu64
               ", bytes_read=%" PRIu64
               ", bytes_overwritten=%" PRIu64
               ", buffer_size=%" PRIu64,
               result.bytes_written, result.bytes_read,
               result.bytes_overwritten, result.buffer_size);
  // After the fix kStall also delivers everything (and was never the broken
  // part anyway — both policies hit the same downstream rejection).
  EXPECT_EQ(result.packets_received, expected);
  EXPECT_EQ(result.trace_writer_packet_loss, 0u);
}

}  // namespace
}  // namespace perfetto
