// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <random>

#include "benchmark/benchmark.h"
#include "perfetto/base/time.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "test/fake_consumer.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"

namespace perfetto {

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#define TEST_CONSUMER_SOCK_NAME "/data/local/tmp/traced_consumer"
#else
#define TEST_PRODUCER_SOCK_NAME PERFETTO_PRODUCER_SOCK_NAME
#define TEST_CONSUMER_SOCK_NAME PERFETTO_CONSUMER_SOCK_NAME
#endif

static void BM_EndToEnd(benchmark::State& state) {
  base::TestTaskRunner task_runner;

  // Setup the TraceConfig for the consumer.
  TraceConfig trace_config;

  // TODO(lalitm): the buffer size should be a function of the benchmark.
  trace_config.add_buffers()->set_size_kb(512);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  // The parameters for the producer.
  static constexpr uint32_t kRandomSeed = 42;
  uint32_t message_count = state.range(0);

  // Setup the test to use a random number generator.
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(message_count);

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread service_thread("perfetto.svc");
  service_thread.Start(std::unique_ptr<ServiceDelegate>(
      new ServiceDelegate(TEST_PRODUCER_SOCK_NAME, TEST_CONSUMER_SOCK_NAME)));
#endif

  TaskRunnerThread producer_thread("perfetto.prd");
  auto on_producer_enabled = task_runner.CreateCheckpoint("producer.enabled");
  auto posted_on_producer_enabled = [&task_runner, &on_producer_enabled] {
    task_runner.PostTask(on_producer_enabled);
  };
  std::unique_ptr<FakeProducerDelegate> producer_delegate(
      new FakeProducerDelegate(TEST_PRODUCER_SOCK_NAME,
                               posted_on_producer_enabled));
  FakeProducerDelegate* producer_delegate_cached = producer_delegate.get();
  producer_thread.Start(std::move(producer_delegate));

  bool is_first_packet = true;
  auto on_readback_complete = task_runner.CreateCheckpoint("readback.complete");
  std::minstd_rand0 rnd_engine(kRandomSeed);
  auto on_consumer_data = [&is_first_packet, &on_readback_complete,
                           &rnd_engine](std::vector<TracePacket> packets,
                                        bool has_more) {
    for (auto& packet : packets) {
      ASSERT_TRUE(packet.Decode());
      ASSERT_TRUE(packet->has_for_testing() || packet->has_clock_snapshot());
      if (packet->has_clock_snapshot()) {
        continue;
      }
      ASSERT_EQ(protos::TracePacket::kTrustedUid,
                packet->optional_trusted_uid_case());
      if (is_first_packet) {
        rnd_engine = std::minstd_rand0(packet->for_testing().seq_value());
        is_first_packet = false;
      } else {
        ASSERT_EQ(packet->for_testing().seq_value(), rnd_engine());
      }
    }

    if (!has_more) {
      is_first_packet = true;
      on_readback_complete();
    }
  };

  // Finally, make the consumer connect to the service.
  auto on_connect = task_runner.CreateCheckpoint("consumer.connected");
  FakeConsumer consumer(trace_config, std::move(on_connect),
                        std::move(on_consumer_data), &task_runner);
  consumer.Connect(TEST_CONSUMER_SOCK_NAME);
  task_runner.RunUntilCheckpoint("consumer.connected");

  consumer.EnableTracing();
  task_runner.RunUntilCheckpoint("producer.enabled");

  uint64_t wall_start_ns = base::GetWallTimeNs().count();
  uint64_t thread_start_ns = service_thread.GetThreadCPUTimeNs();
  while (state.KeepRunning()) {
    auto cname = "produced.and.committed." + std::to_string(state.iterations());
    auto on_produced_and_committed = task_runner.CreateCheckpoint(cname);
    auto posted_on_produced_and_committed = [&task_runner,
                                             &on_produced_and_committed] {
      task_runner.PostTask(on_produced_and_committed);
    };
    FakeProducer* producer = producer_delegate_cached->producer();
    producer->ProduceEventBatch(posted_on_produced_and_committed);
    task_runner.RunUntilCheckpoint(cname);
  }
  uint64_t thread_ns = service_thread.GetThreadCPUTimeNs() - thread_start_ns;
  uint64_t wall_ns = base::GetWallTimeNs().count() - wall_start_ns;
  PERFETTO_ILOG("Service CPU usage: %.2f,  CPU/iterations: %lf",
                100.0 * thread_ns / wall_ns, 1.0 * thread_ns / message_count);

  // Read back the buffer just to check correctness.
  consumer.ReadTraceData();
  task_runner.RunUntilCheckpoint("readback.complete");
  state.SetBytesProcessed(int64_t(state.iterations()) *
                          (sizeof(uint32_t) + 1024) * message_count);

  consumer.Disconnect();
}

BENCHMARK(BM_EndToEnd)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime()
    ->RangeMultiplier(2)
    ->Range(16, 1024 * 1024);
}
