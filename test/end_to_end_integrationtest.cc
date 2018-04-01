/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <random>
#include <thread>

#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"
#include "test/test_helper.h"

namespace perfetto {

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#else
#define TEST_PRODUCER_SOCK_NAME PERFETTO_PRODUCER_SOCK_NAME
#endif

// TODO(b/73453011): reenable this on more platforms (including standalone
// Android).
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#define MAYBE_TestFtraceProducer TestFtraceProducer
#else
#define MAYBE_TestFtraceProducer DISABLED_TestFtraceProducer
#endif
TEST(PerfettoTest, MAYBE_TestFtraceProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  helper.ConnectConsumer();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");
  ds_config->set_target_buffer(0);

  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_ftrace_events() = "sched_switch";
  *ftrace_config->add_ftrace_events() = "bar";

  auto producer_enabled = task_runner.CreateCheckpoint("producer.enabled");
  task_runner.PostDelayedTask(producer_enabled, 100);
  helper.StartTracing(trace_config);

  size_t packets_seen = 0;
  auto on_consumer_data =
      [&packets_seen](const TracePacket::DecodedTracePacket& packet) {
        for (int ev = 0; ev < packet.ftrace_events().event_size(); ev++) {
          ASSERT_TRUE(packet.ftrace_events().event(ev).has_sched_switch());
        }
        packets_seen++;
      };
  auto on_readback_complete = task_runner.CreateCheckpoint("readback.complete");
  task_runner.PostDelayedTask(
      [&helper, &on_consumer_data, &on_readback_complete] {
        helper.ReadData(on_consumer_data, on_readback_complete);
      },
      3000);
  task_runner.RunUntilCheckpoint("readback.complete");
  ASSERT_GT(packets_seen, 0u);
}

TEST(PerfettoTest, TestFakeProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

  FakeProducer* producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 10;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);

  helper.StartTracing(trace_config);

  producer->ProduceEventBatch(
      helper.WrapTask(task_runner.CreateCheckpoint("produced.and.committed")));
  task_runner.RunUntilCheckpoint("produced.and.committed");

  size_t packets_seen = 0;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  auto on_consumer_data = [&packets_seen, &rnd_engine](
                              const TracePacket::DecodedTracePacket& packet) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
    packets_seen++;
  };
  auto on_readback_complete = task_runner.CreateCheckpoint("readback.complete");
  helper.ReadData(on_consumer_data, on_readback_complete);
  task_runner.RunUntilCheckpoint("readback.complete");
  ASSERT_EQ(packets_seen, kNumPackets);
}

TEST(PerfettoTest, VeryLargePackets) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

  FakeProducer* producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();

  // Setup the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 5;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024 * 1024 - 42;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);

  helper.StartTracing(trace_config);

  producer->ProduceEventBatch(
      helper.WrapTask(task_runner.CreateCheckpoint("produced.and.committed")));
  task_runner.RunUntilCheckpoint("produced.and.committed");

  size_t packets_seen = 0;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  auto on_consumer_data = [&packets_seen, &rnd_engine](
                              const TracePacket::DecodedTracePacket& packet) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
    size_t msg_size = packet.for_testing().str().size();
    ASSERT_EQ(kMsgSize, msg_size);
    for (size_t i = 0; i < msg_size; i++)
      ASSERT_EQ(i < msg_size - 1 ? '.' : 0, packet.for_testing().str()[i]);
    packets_seen++;
  };
  auto on_readback_complete = task_runner.CreateCheckpoint("readback.complete");
  helper.ReadData(on_consumer_data, on_readback_complete);
  task_runner.RunUntilCheckpoint("readback.complete");
  ASSERT_EQ(packets_seen, kNumPackets);
}

}  // namespace perfetto
