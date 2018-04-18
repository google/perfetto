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
#include "perfetto/base/time.h"
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
  endpoint_ = ProducerIPCClient::Connect(
      socket_name, this, "android.perfetto.FakeProducer", task_runner);
  on_create_data_source_instance_ = std::move(on_create_data_source_instance);
}

void FakeProducer::OnConnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  DataSourceDescriptor descriptor;
  descriptor.set_name(name_);
  endpoint_->RegisterDataSource(descriptor);
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
  max_messages_per_second_ =
      source_config.for_testing().max_messages_per_second();
  if (source_config.for_testing().send_batch_on_register()) {
    ProduceEventBatch(on_create_data_source_instance_);
  } else {
    task_runner_->PostTask(on_create_data_source_instance_);
  }
}

void FakeProducer::TearDownDataSourceInstance(DataSourceInstanceID) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  trace_writer_.reset();
}

// Note: this can be called on a different thread.
void FakeProducer::ProduceEventBatch(std::function<void()> callback) {
  task_runner_->PostTask([this, callback] {
    PERFETTO_CHECK(trace_writer_);
    PERFETTO_CHECK(message_size_ > 1);
    std::unique_ptr<char, base::FreeDeleter> payload(
        static_cast<char*>(malloc(message_size_)));
    memset(payload.get(), '.', message_size_);
    payload.get()[message_size_ - 1] = 0;

    base::TimeMillis start = base::GetWallTimeMs();
    int64_t iterations = 0;
    uint32_t messages_to_emit = message_count_;
    while (messages_to_emit > 0) {
      uint32_t messages_in_minibatch =
          max_messages_per_second_ == 0
              ? messages_to_emit
              : std::min(max_messages_per_second_, messages_to_emit);
      PERFETTO_DCHECK(messages_to_emit >= messages_in_minibatch);

      for (uint32_t i = 0; i < messages_in_minibatch; i++) {
        auto handle = trace_writer_->NewTracePacket();
        handle->set_for_testing()->set_seq_value(
            static_cast<uint32_t>(rnd_engine_()));
        handle->set_for_testing()->set_str(payload.get(), message_size_);
      }
      messages_to_emit -= messages_in_minibatch;
      iterations++;

      // Pause until the second boundary to make sure that we are adhering to
      // the speed limitation.
      if (max_messages_per_second_ > 0) {
        int64_t expected_time_taken = iterations * 1000;
        base::TimeMillis time_taken = base::GetWallTimeMs() - start;
        while (time_taken.count() < expected_time_taken) {
          usleep(static_cast<useconds_t>(
              (expected_time_taken - time_taken.count()) * 1000));
          time_taken = base::GetWallTimeMs() - start;
        }
      }
      trace_writer_->Flush(messages_to_emit > 0 ? [] {} : callback);
    }
  });
}

void FakeProducer::OnTracingSetup() {}

void FakeProducer::Flush(FlushRequestID flush_request_id,
                         const DataSourceInstanceID*,
                         size_t num_data_sources) {
  PERFETTO_DCHECK(num_data_sources > 0);
  if (trace_writer_)
    trace_writer_->Flush();
  endpoint_->NotifyFlushComplete(flush_request_id);
}

}  // namespace perfetto
