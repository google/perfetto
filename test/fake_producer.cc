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

#include <condition_variable>
#include <mutex>

#include "gtest/gtest.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"

namespace perfetto {

FakeProducer::FakeProducer(const std::string& name) : name_(name) {}
FakeProducer::~FakeProducer() = default;

void FakeProducer::Connect(
    const char* socket_name,
    base::TaskRunner* task_runner,
    std::function<void()> on_create_data_source_instance) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  task_runner_ = task_runner;
  endpoint_ = ProducerIPCClient::Connect(socket_name, this, task_runner);
  on_create_data_source_instance_ = std::move(on_create_data_source_instance);
}

void FakeProducer::OnConnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  DataSourceDescriptor descriptor;
  descriptor.set_name(name_);
  endpoint_->RegisterDataSource(descriptor,
                                [this](DataSourceID id) { id_ = id; });
}

void FakeProducer::OnDisconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  FAIL() << "Producer unexpectedly disconnected from the service";
}

void FakeProducer::CreateDataSourceInstance(
    DataSourceInstanceID,
    const DataSourceConfig& source_config) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  trace_writer_ = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  rnd_engine_ = std::minstd_rand0(source_config.for_testing().seed());
  message_count_ = source_config.for_testing().message_count();
  message_size_ = source_config.for_testing().message_size();
  task_runner_->PostTask(on_create_data_source_instance_);
}

void FakeProducer::TearDownDataSourceInstance(DataSourceInstanceID) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  trace_writer_.reset();
}

// Note: this will called on a different thread.
void FakeProducer::ProduceEventBatch(std::function<void()> callback) {
  task_runner_->PostTask([this, callback] {
    PERFETTO_CHECK(trace_writer_);

    size_t payload_size = message_size_ - sizeof(uint32_t);
    PERFETTO_CHECK(payload_size >= sizeof(char));

    std::unique_ptr<char, base::FreeDeleter> payload(
        static_cast<char*>(malloc(payload_size)));
    memset(payload.get(), '.', payload_size);
    payload.get()[payload_size - 1] = 0;
    for (size_t i = 0; i < message_count_; i++) {
      auto handle = trace_writer_->NewTracePacket();
      handle->set_for_testing()->set_seq_value(rnd_engine_());
      handle->set_for_testing()->set_str(payload.get(), payload_size);
    }
    trace_writer_->Flush(callback);
  });
}

void FakeProducer::OnTracingStart() {}

void FakeProducer::OnTracingStop() {}

}  // namespace perfetto
