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

#include "test/fake_producer.h"

#include <random>

#include "perfetto/base/logging.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"

namespace perfetto {

FakeProducer::FakeProducer(const std::string& name) : name_(name) {}
FakeProducer::~FakeProducer() = default;

void FakeProducer::Connect(const char* socket_name,
                           base::TaskRunner* task_runner,
                           std::function<void()> data_produced_callback) {
  task_runner_ = task_runner;
  data_produced_callback_ = std::move(data_produced_callback);
  endpoint_ = ProducerIPCClient::Connect(socket_name, this, task_runner);
}

void FakeProducer::OnConnect() {
  DataSourceDescriptor descriptor;
  descriptor.set_name(name_);
  endpoint_->RegisterDataSource(descriptor,
                                [this](DataSourceID id) { id_ = id; });
}

void FakeProducer::OnDisconnect() {}

void FakeProducer::CreateDataSourceInstance(
    DataSourceInstanceID,
    const DataSourceConfig& source_config) {
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));

  const TestConfig& config = source_config.for_testing();
  std::minstd_rand0 random(config.seed());
  for (size_t i = 0; i < config.message_count(); i++) {
    auto handle = trace_writer->NewTracePacket();
    handle->set_for_testing()->set_seq_value(random());
    handle->Finalize();
  }

  // TODO(primiano): reenable this once UnregisterDataSource is specified in
  // ServiceImpl.
  // endpoint_->UnregisterDataSource(id_);

  // TODO(skyostil): There's a race here before the service processes our data
  // and the consumer tries to retrieve it. For now wait a bit until the service
  // is done, but we should add explicit flushing to avoid this.
  task_runner_->PostDelayedTask(data_produced_callback_, 1000);
}

void FakeProducer::TearDownDataSourceInstance(DataSourceInstanceID) {}

}  // namespace perfetto
