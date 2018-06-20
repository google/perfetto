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

#ifndef TEST_TEST_HELPER_H_
#define TEST_TEST_HELPER_H_

#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"
#include "src/base/test/test_task_runner.h"
#include "test/fake_producer.h"
#include "test/task_runner_thread.h"

#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {

class TestHelper : public Consumer {
 public:
  explicit TestHelper(base::TestTaskRunner* task_runner);

  // Consumer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTracingDisabled() override;
  void OnTraceData(std::vector<TracePacket> packets, bool has_more) override;

  void StartServiceIfRequired();
  FakeProducer* ConnectFakeProducer();
  void ConnectConsumer();
  void StartTracing(const TraceConfig& config);
  void ReadData(uint32_t read_count = 0);

  void WaitForConsumerConnect();
  void WaitForProducerEnabled();
  void WaitForTracingDisabled();
  void WaitForReadData(uint32_t read_count = 0);

  std::function<void()> WrapTask(const std::function<void()>& function);

  TaskRunnerThread* service_thread() { return &service_thread_; }
  TaskRunnerThread* producer_thread() { return &producer_thread_; }
  const std::vector<protos::TracePacket>& trace() { return trace_; }

 private:
  base::TestTaskRunner* task_runner_ = nullptr;

  std::function<void()> on_connect_callback_;
  std::function<void()> on_packets_finished_callback_;
  std::function<void()> on_stop_tracing_callback_;

  std::vector<protos::TracePacket> trace_;

  TaskRunnerThread service_thread_;
  TaskRunnerThread producer_thread_;
  std::unique_ptr<TracingService::ConsumerEndpoint> endpoint_;  // Keep last.
};

}  // namespace perfetto

#endif  // TEST_TEST_HELPER_H_
