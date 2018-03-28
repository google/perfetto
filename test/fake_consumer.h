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

#ifndef TEST_FAKE_CONSUMER_H_
#define TEST_FAKE_CONSUMER_H_

#include <memory>
#include <vector>

#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"

#include "src/base/test/test_task_runner.h"

namespace perfetto {

class FakeConsumer : public Consumer {
 public:
  FakeConsumer(
      const TraceConfig& trace_config,
      std::function<void()> on_connect,
      std::function<void(std::vector<TracePacket>, bool)> packet_callback,
      base::TaskRunner* task_runner);
  ~FakeConsumer() override;

  void EnableTracing();
  void FreeBuffers();
  void Connect(const char* socket_name);
  void Disconnect();
  void ReadTraceData();

  // Consumer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTracingStop() override;
  void OnTraceData(std::vector<TracePacket> packets, bool has_more) override;

 private:
  base::TaskRunner* const task_runner_;
  const TraceConfig trace_config_;
  std::function<void()> on_connect_;
  std::function<void(std::vector<TracePacket>, bool)> packet_callback_;
  std::unique_ptr<Service::ConsumerEndpoint> endpoint_;  // Keep last.
};

}  // namespace perfetto

#endif  // TEST_FAKE_CONSUMER_H_
