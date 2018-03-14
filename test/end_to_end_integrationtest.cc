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

#include <gtest/gtest.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <random>
#include <thread>

#include "perfetto/base/logging.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"
#include "src/base/test/test_task_runner.h"
#include "test/fake_consumer.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include "perfetto/base/android_task_runner.h"
#endif

namespace perfetto {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
using PlatformTaskRunner = base::AndroidTaskRunner;
#else
using PlatformTaskRunner = base::UnixTaskRunner;
#endif

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

class PerfettoTest : public ::testing::Test {
 public:
  PerfettoTest() {}
  ~PerfettoTest() override = default;
};

// TODO(b/73453011): reenable this on more platforms (including standalone
// Android).
#if defined(PERFETTO_BUILD_WITH_ANDROID)
#define MAYBE_TestFtraceProducer TestFtraceProducer
#else
#define MAYBE_TestFtraceProducer DISABLED_TestFtraceProducer
#endif
TEST_F(PerfettoTest, MAYBE_TestFtraceProducer) {
  base::TestTaskRunner task_runner;
  auto finish = task_runner.CreateCheckpoint("no.more.packets");

  // Setip the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(10000);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("com.google.perfetto.ftrace");
  ds_config->set_target_buffer(0);

  // Setup the config for ftrace.
  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_ftrace_events() = "sched_switch";
  *ftrace_config->add_ftrace_events() = "bar";

  // Create the function to handle packets as they come in.
  uint64_t total = 0;
  auto function = [&total, &finish](std::vector<TracePacket> packets,
                                    bool has_more) {
    for (auto& packet : packets) {
      ASSERT_TRUE(packet.Decode());
      ASSERT_TRUE(packet->has_ftrace_events());
      for (int ev = 0; ev < packet->ftrace_events().event_size(); ev++) {
        ASSERT_TRUE(packet->ftrace_events().event(ev).has_sched_switch());
      }
    }
    total += packets.size();

    if (!has_more) {
      ASSERT_GE(total, static_cast<uint64_t>(sysconf(_SC_NPROCESSORS_CONF)));
      finish();
    }
  };

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread service_thread;
  service_thread.Start(std::unique_ptr<ServiceDelegate>(
      new ServiceDelegate(TEST_PRODUCER_SOCK_NAME, TEST_CONSUMER_SOCK_NAME)));

  TaskRunnerThread producer_thread;
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  // Finally, make the consumer connect to the service.
  FakeConsumer consumer(trace_config, std::move(function), &task_runner);
  consumer.Connect(TEST_CONSUMER_SOCK_NAME);

  // TODO(skyostil): There's a race here before the service processes our data
  // and the consumer tries to retrieve it. For now wait a bit until the service
  // is done, but we should add explicit flushing to avoid this.
  task_runner.PostDelayedTask([&consumer]() { consumer.ReadTraceData(); },
                              13000);

  task_runner.RunUntilCheckpoint("no.more.packets", 20000);
}

// TODO(b/73453011): reenable this on more platforms (including standalone
// Android).
#if defined(PERFETTO_BUILD_WITH_ANDROID)
#define MAYBE_TestFakeProducer TestFakeProducer
#else
#define MAYBE_TestFakeProducer DISABLED_TestFakeProducer
#endif
TEST_F(PerfettoTest, MAYBE_TestFakeProducer) {
  base::TestTaskRunner task_runner;
  auto finish = task_runner.CreateCheckpoint("no.more.packets");

  // Setup the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(200);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  // The parameters for the producer.
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kEventCount = 10;

  // Setup the test to use a random number generator.
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kEventCount);

  // Create the random generator with the same seed.
  std::minstd_rand0 random(kRandomSeed);

  // Create the function to handle packets as they come in.
  uint64_t total = 0;
  auto function = [&total, &finish, &random](std::vector<TracePacket> packets,
                                             bool has_more) {

    for (auto& packet : packets) {
      ASSERT_TRUE(packet.Decode());
      ASSERT_TRUE(packet->has_for_testing());
      ASSERT_EQ(protos::TracePacket::kTrustedUid,
                packet->optional_trusted_uid_case());
      ASSERT_EQ(packet->for_testing().seq_value(), random());
    }
    total += packets.size();

    if (!has_more) {
      ASSERT_EQ(total, kEventCount);
      finish();
    }
  };

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread service_thread;
  service_thread.Start(std::unique_ptr<ServiceDelegate>(
      new ServiceDelegate(TEST_PRODUCER_SOCK_NAME, TEST_CONSUMER_SOCK_NAME)));
#endif

  auto data_produced = task_runner.CreateCheckpoint("data.produced");
  TaskRunnerThread producer_thread;
  producer_thread.Start(
      std::unique_ptr<FakeProducerDelegate>(new FakeProducerDelegate(
          TEST_PRODUCER_SOCK_NAME, [&task_runner, &data_produced] {
            task_runner.PostTask(data_produced);
          })));

  // Finally, make the consumer connect to the service.
  FakeConsumer consumer(trace_config, std::move(function), &task_runner);
  consumer.Connect(TEST_CONSUMER_SOCK_NAME);

  task_runner.RunUntilCheckpoint("data.produced");
  consumer.ReadTraceData();

  task_runner.RunUntilCheckpoint("no.more.packets");
}

}  // namespace perfetto
