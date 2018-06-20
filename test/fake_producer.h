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

#ifndef TEST_FAKE_PRODUCER_H_
#define TEST_FAKE_PRODUCER_H_

#include <memory>
#include <random>
#include <string>

#include "perfetto/base/thread_checker.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"
#include "src/base/test/test_task_runner.h"

namespace perfetto {

class FakeProducer : public Producer {
 public:
  explicit FakeProducer(const std::string& name);
  ~FakeProducer() override;

  void Connect(const char* socket_name,
               base::TaskRunner* task_runner,
               std::function<void()> on_create_data_source_instance);

  // Produces a batch of events (as configured in the DataSourceConfig) and
  // posts a callback when the service acknowledges the commit.
  void ProduceEventBatch(std::function<void()> callback = [] {});

  // Producer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void CreateDataSourceInstance(DataSourceInstanceID,
                                const DataSourceConfig& source_config) override;
  void TearDownDataSourceInstance(DataSourceInstanceID) override;
  void OnTracingSetup() override;
  void Flush(FlushRequestID, const DataSourceInstanceID*, size_t) override;

 private:
  void Shutdown();

  base::ThreadChecker thread_checker_;
  base::TaskRunner* task_runner_ = nullptr;
  std::string name_;
  std::minstd_rand0 rnd_engine_;
  uint32_t message_size_ = 0;
  uint32_t message_count_ = 0;
  uint32_t max_messages_per_second_ = 0;
  std::function<void()> on_create_data_source_instance_;
  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;
  std::unique_ptr<TraceWriter> trace_writer_;
};

}  // namespace perfetto

#endif  // TEST_FAKE_PRODUCER_H_
