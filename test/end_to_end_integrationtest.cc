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
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/tracing/ipc/default_socket.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"
#include "test/test_helper.h"

#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

class PerfettoTest : public ::testing::Test {
 public:
  void SetUp() override {
    // TODO(primiano): refactor this, it's copy/pasted in three places now.
    size_t index = 0;
    constexpr auto kTracingPaths = FtraceController::kTracingPaths;
    while (!ftrace_procfs_ && kTracingPaths[index]) {
      ftrace_procfs_ = FtraceProcfs::Create(kTracingPaths[index++]);
    }
    if (!ftrace_procfs_)
      return;
    ftrace_procfs_->SetTracingOn(false);
  }

  void TearDown() override {
    if (ftrace_procfs_)
      ftrace_procfs_->SetTracingOn(false);
  }

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
};

}  // namespace

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#else
#define TEST_PRODUCER_SOCK_NAME ::perfetto::GetProducerSocket()
#endif

// TODO(b/73453011): reenable on more platforms (including standalone Android).
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#define TreeHuggerOnly(x) x
#else
#define TreeHuggerOnly(x) DISABLED_##x
#endif

TEST_F(PerfettoTest, TreeHuggerOnly(TestFtraceProducer)) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");
  ds_config->set_target_buffer(0);

  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_ftrace_events() = "sched_switch";
  *ftrace_config->add_ftrace_events() = "bar";

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  for (const auto& packet : packets) {
    for (int ev = 0; ev < packet.ftrace_events().event_size(); ev++) {
      ASSERT_TRUE(packet.ftrace_events().event(ev).has_sched_switch());
    }
  }
}

TEST_F(PerfettoTest, TreeHuggerOnly(TestFtraceFlush)) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  const uint32_t kTestTimeoutMs = 30000;
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(16);
  trace_config.set_duration_ms(kTestTimeoutMs);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");

  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_ftrace_events() = "print";

  helper.StartTracing(trace_config);

  // Do a first flush just to synchronize with the producer. The problem here
  // is that, on a Linux workstation, the producer can take several seconds just
  // to get to the point where ftrace is ready. We use the flush ack as a
  // synchronization point.
  helper.FlushAndWait(kTestTimeoutMs);

  EXPECT_TRUE(ftrace_procfs_->IsTracingEnabled());
  const char kMarker[] = "just_one_event";
  EXPECT_TRUE(ftrace_procfs_->WriteTraceMarker(kMarker));

  // This is the real flush we are testing.
  helper.FlushAndWait(kTestTimeoutMs);

  helper.DisableTracing();
  helper.WaitForTracingDisabled(kTestTimeoutMs);

  helper.ReadData();
  helper.WaitForReadData();

  int marker_found = 0;
  for (const auto& packet : helper.trace()) {
    for (int i = 0; i < packet.ftrace_events().event_size(); i++) {
      const auto& ev = packet.ftrace_events().event(i);
      if (ev.has_print() && ev.print().buf().find(kMarker) != std::string::npos)
        marker_found++;
    }
  }
  ASSERT_EQ(marker_found, 1);
}

TEST_F(PerfettoTest, TreeHuggerOnly(TestBatteryTracing)) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.power");
  ds_config->set_target_buffer(0);
  auto* power_config = ds_config->mutable_android_power_config();
  power_config->set_battery_poll_ms(250);
  *power_config->add_battery_counters() =
      AndroidPowerConfig::BATTERY_COUNTER_CHARGE;
  *power_config->add_battery_counters() =
      AndroidPowerConfig::BATTERY_COUNTER_CAPACITY_PERCENT;

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  bool has_battery_packet = false;
  for (const auto& packet : packets) {
    if (!packet.has_battery())
      continue;
    has_battery_packet = true;
    EXPECT_GE(packet.battery().charge_counter_uah(), 0);
    EXPECT_GE(packet.battery().capacity_percent(), 0);
    EXPECT_LE(packet.battery().capacity_percent(), 100);
  }

  ASSERT_TRUE(has_battery_packet);
}

TEST_F(PerfettoTest, TestFakeProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

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
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
  }
}

TEST_F(PerfettoTest, VeryLargePackets) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(500);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 5;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024 * 1024 - 42;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
    size_t msg_size = packet.for_testing().str().size();
    ASSERT_EQ(kMsgSize, msg_size);
    for (size_t i = 0; i < msg_size; i++)
      ASSERT_EQ(i < msg_size - 1 ? '.' : 0, packet.for_testing().str()[i]);
  }
}

}  // namespace perfetto
