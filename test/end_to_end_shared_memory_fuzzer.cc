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

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/utils.h"
#include "perfetto/ipc/host.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"
#include "perfetto/tracing/ipc/service_ipc_host.h"
#include "src/base/test/test_task_runner.h"
#include "test/fake_consumer.h"
#include "test/task_runner_thread.h"

namespace perfetto {
namespace shm_fuzz {

static const char* kProducerSocket = tempnam("/tmp", "perfetto-producer");
static const char* kConsumerSocket = tempnam("/tmp", "perfetto-consumer");

// Fake producer writing a protozero message of data into shared memory
// buffer, followed by a sentinel message to signal completion to the
// consumer.
class FakeProducer : public Producer {
 public:
  FakeProducer(std::string name,
               const uint8_t* data,
               size_t size,
               FakeConsumer* consumer)
      : name_(std::move(name)), data_(data), size_(size), consumer_(consumer) {}

  void Connect(const char* socket_name, base::TaskRunner* task_runner) {
    endpoint_ = ProducerIPCClient::Connect(socket_name, this, task_runner);
  }

  void OnConnect() override {
    DataSourceDescriptor descriptor;
    descriptor.set_name(name_);
    endpoint_->RegisterDataSource(descriptor,
                                  [this](DataSourceID id) { id_ = id; });
  }

  void OnDisconnect() override {}

  void CreateDataSourceInstance(
      DataSourceInstanceID,
      const DataSourceConfig& source_config) override {
    // The block is to destroy |packet| and |trace_writer| in order. Destroying
    // the |trace_writer| will cause a flush of the completed packets.
    {
      auto trace_writer = endpoint_->CreateTraceWriter(
          static_cast<BufferID>(source_config.target_buffer()));
      auto packet = trace_writer->NewTracePacket();
      packet->stream_writer_->WriteBytes(data_, size_);
      packet->Finalize();
    }
    {
      auto trace_writer = endpoint_->CreateTraceWriter(
          static_cast<BufferID>(source_config.target_buffer()));
      auto end_packet = trace_writer->NewTracePacket();
      end_packet->set_for_testing()->set_str("end");
      end_packet->Finalize();
    }
    consumer_->BusyWaitReadBuffers();
  }

  void TearDownDataSourceInstance(DataSourceInstanceID) override {}

 private:
  const std::string name_;
  const uint8_t* data_;
  const size_t size_;
  DataSourceID id_ = 0;
  std::unique_ptr<Service::ProducerEndpoint> endpoint_;
  FakeConsumer* consumer_;
};

class FakeProducerDelegate : public ThreadDelegate {
 public:
  FakeProducerDelegate(const uint8_t* data, size_t size, FakeConsumer* consumer)
      : data_(data), size_(size), consumer_(consumer) {}
  ~FakeProducerDelegate() override = default;

  void Initialize(base::TaskRunner* task_runner) override {
    producer_.reset(new FakeProducer("android.perfetto.FakeProducer", data_,
                                     size_, consumer_));
    producer_->Connect(kProducerSocket, task_runner);
  }

 private:
  std::unique_ptr<FakeProducer> producer_;
  const uint8_t* data_;
  const size_t size_;
  FakeConsumer* consumer_;
};

class ServiceDelegate : public ThreadDelegate {
 public:
  ServiceDelegate() = default;
  ~ServiceDelegate() override = default;
  void Initialize(base::TaskRunner* task_runner) override {
    svc_ = ServiceIPCHost::CreateInstance(task_runner);
    unlink(kProducerSocket);
    unlink(kConsumerSocket);
    svc_->Start(kProducerSocket, kConsumerSocket);
  }

 private:
  std::unique_ptr<ServiceIPCHost> svc_;
  base::ScopedFile producer_fd_;
  base::ScopedFile consumer_fd_;
};

int FuzzSharedMemory(const uint8_t* data, size_t size);

int FuzzSharedMemory(const uint8_t* data, size_t size) {
  TaskRunnerThread service_thread;
  service_thread.Start(std::unique_ptr<ServiceDelegate>(new ServiceDelegate()));

  // Setup the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(8);
  trace_config.set_duration_ms(1000);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  base::TestTaskRunner task_runner;
  auto finish = task_runner.CreateCheckpoint("no.more.packets");
  // Wait for sentinel message from Producer, then signal no.more.packets.
  auto function = [&finish](std::vector<TracePacket> packets, bool has_more) {
    for (auto& p : packets) {
      p.Decode();
      if (p->for_testing().str() == "end")
        finish();
    }
  };
  FakeConsumer consumer(trace_config, std::move(function), &task_runner);
  consumer.Connect(kConsumerSocket);

  TaskRunnerThread producer_thread;
  producer_thread.Start(std::unique_ptr<FakeProducerDelegate>(
      new FakeProducerDelegate(data, size, &consumer)));
  task_runner.RunUntilCheckpoint("no.more.packets");
  return 0;
}

}  // namespace shm_fuzz
}  // namespace perfetto

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return perfetto::shm_fuzz::FuzzSharedMemory(data, size);
}
