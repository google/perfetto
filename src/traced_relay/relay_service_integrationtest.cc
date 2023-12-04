/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <memory>
#include "src/traced_relay/relay_service.h"

#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"

namespace perfetto {
namespace {

TEST(TracedRelayIntegrationTest, BasicCase) {
  base::TestTaskRunner task_runner;

  std::string sock_name;
  {
    // Set up a server UnixSocket to find an unused TCP port.
    base::UnixSocket::EventListener event_listener;
    auto srv = base::UnixSocket::Listen("127.0.0.1:0", &event_listener,
                                        &task_runner, base::SockFamily::kInet,
                                        base::SockType::kStream);
    ASSERT_TRUE(srv->is_listening());
    sock_name = srv->GetSockAddr();
    // Shut down |srv| here to free the port. It's unlikely that the port will
    // be taken by another process so quickly before we reach the code below.
  }

  TestHelper helper(&task_runner, TestHelper::Mode::kStartDaemons,
                    sock_name.c_str());
  ASSERT_EQ(helper.num_producers(), 1u);
  helper.StartServiceIfRequired();

  auto relay_service = std::make_unique<RelayService>(&task_runner);

  relay_service->Start("@traced_relay", sock_name.c_str());

  auto producer_connected =
      task_runner.CreateCheckpoint("perfetto.FakeProducer.connected");
  auto noop = []() {};
  auto connected = [&]() { task_runner.PostTask(producer_connected); };

  // We won't use the built-in fake producer and will start our own.
  auto producer_thread = std::make_unique<FakeProducerThread>(
      "@traced_relay", connected, noop, noop, "perfetto.FakeProducer");
  producer_thread->Connect();
  task_runner.RunUntilCheckpoint("perfetto.FakeProducer.connected");

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  static constexpr uint32_t kMsgSize = 1024;
  static constexpr uint32_t kRandomSeed = 42;
  // Enable the producer.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.FakeProducer");
  ds_config->set_target_buffer(0);
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(12);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), 12u);

  // The producer is connected from this process. The relay service will inject
  // the SetPeerIdentity message using the pid and euid of the current process.
  auto pid = static_cast<int32_t>(getpid());
  auto uid = static_cast<int32_t>(geteuid());

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.trusted_pid(), pid);
    ASSERT_EQ(packet.trusted_uid(), uid);
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
  }
}

}  // namespace
}  // namespace perfetto
