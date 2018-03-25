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

#include <random>

#include "gtest/gtest.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "test/fake_consumer.h"

namespace perfetto {

class PerfettoCtsTest : public ::testing::Test {
 protected:
  void TestMockProducer(const std::string& producer_name) {
    base::TestTaskRunner task_runner;

    // Setup the trace config.
    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(4096 * 10);
    trace_config.set_duration_ms(200);

    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name(producer_name);
    ds_config->set_target_buffer(0);

    // The parameters for the producer.
    static constexpr uint32_t kRandomSeed = 42;
    static constexpr uint32_t kEventCount = 10;

    // Setup the test to use a random number generator.
    ds_config->mutable_for_testing()->set_seed(kRandomSeed);
    ds_config->mutable_for_testing()->set_message_count(kEventCount);

    // Create the random generator with the same seed.
    std::minstd_rand0 rnd_engine(kRandomSeed);

    // Setip the function.
    uint64_t total = 0;
    auto finish = task_runner.CreateCheckpoint("no.more.packets");
    auto function = [&total, &finish, &rnd_engine](
                        std::vector<TracePacket> packets, bool has_more) {
      for (auto& packet : packets) {
        ASSERT_TRUE(packet.Decode());
        ASSERT_TRUE(packet->has_for_testing() || packet->has_clock_snapshot());
        if (packet->has_clock_snapshot()) {
          continue;
        }
        ASSERT_EQ(protos::TracePacket::kTrustedUid,
                  packet->optional_trusted_uid_case());
        ASSERT_EQ(packet->for_testing().seq_value(), rnd_engine());
      }
      total += packets.size();

      if (!has_more) {
        ASSERT_EQ(total, kEventCount + 1);
        finish();
      }
    };

    // Finally, make the consumer connect to the service.
    auto on_connect = task_runner.CreateCheckpoint("consumer.connected");
    FakeConsumer consumer(trace_config, std::move(on_connect),
                          std::move(function), &task_runner);
    consumer.Connect(PERFETTO_CONSUMER_SOCK_NAME);

    task_runner.RunUntilCheckpoint("consumer.connected");
    consumer.EnableTracing();

    // TODO(skyostil): There's a race here before the service processes our data
    // and the consumer tries to retrieve it. For now wait a bit until the
    // service is done, but we should add explicit flushing to avoid this.
    task_runner.PostDelayedTask([&consumer]() { consumer.ReadTraceData(); },
                                1000);

    task_runner.RunUntilCheckpoint("no.more.packets");
  }
};

TEST_F(PerfettoCtsTest, TestProducerActivity) {
  TestMockProducer("android.perfetto.cts.ProducerActivity");
}

TEST_F(PerfettoCtsTest, TestProducerService) {
  TestMockProducer("android.perfetto.cts.ProducerService");
}

TEST_F(PerfettoCtsTest, TestProducerIsolatedService) {
  TestMockProducer("android.perfetto.cts.ProducerIsolatedService");
}

}  // namespace perfetto
