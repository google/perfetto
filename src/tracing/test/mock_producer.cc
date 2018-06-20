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

#include "src/tracing/test/mock_producer.h"

#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Property;

namespace perfetto {

MockProducer::MockProducer(base::TestTaskRunner* task_runner)
    : task_runner_(task_runner) {}

MockProducer::~MockProducer() {
  if (!service_endpoint_)
    return;
  static int i = 0;
  auto checkpoint_name = "on_producer_disconnect_" + std::to_string(i++);
  auto on_disconnect = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnDisconnect()).WillOnce(Invoke(on_disconnect));
  service_endpoint_.reset();
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

void MockProducer::Connect(TracingService* svc,
                           const std::string& producer_name,
                           uid_t uid,
                           size_t shared_memory_size_hint_bytes) {
  producer_name_ = producer_name;
  service_endpoint_ = svc->ConnectProducer(this, uid, producer_name,
                                           shared_memory_size_hint_bytes);
  auto checkpoint_name = "on_producer_connect_" + producer_name;
  auto on_connect = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnConnect()).WillOnce(Invoke(on_connect));
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

void MockProducer::RegisterDataSource(const std::string& name) {
  DataSourceDescriptor ds_desc;
  ds_desc.set_name(name);
  service_endpoint_->RegisterDataSource(ds_desc);
}

void MockProducer::UnregisterDataSource(const std::string& name) {
  service_endpoint_->UnregisterDataSource(name);
}

void MockProducer::WaitForTracingSetup() {
  static int i = 0;
  auto checkpoint_name =
      "on_shmem_initialized_" + producer_name_ + "_" + std::to_string(i++);
  auto on_tracing_enabled = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnTracingSetup()).WillOnce(Invoke(on_tracing_enabled));
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

void MockProducer::WaitForDataSourceStart(const std::string& name) {
  static int i = 0;
  auto checkpoint_name = "on_ds_start_" + name + "_" + std::to_string(i++);
  auto on_ds_start = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, CreateDataSourceInstance(
                         _, Property(&DataSourceConfig::name, Eq(name))))
      .WillOnce(Invoke([on_ds_start, this](DataSourceInstanceID ds_id,
                                           const DataSourceConfig& cfg) {
        EXPECT_FALSE(data_source_instances_.count(cfg.name()));
        auto target_buffer = static_cast<BufferID>(cfg.target_buffer());
        data_source_instances_.emplace(cfg.name(),
                                       EnabledDataSource{ds_id, target_buffer});
        on_ds_start();
      }));
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

void MockProducer::WaitForDataSourceStop(const std::string& name) {
  static int i = 0;
  auto checkpoint_name = "on_ds_stop_" + name + "_" + std::to_string(i++);
  auto on_ds_stop = task_runner_->CreateCheckpoint(checkpoint_name);
  ASSERT_EQ(1u, data_source_instances_.count(name));
  DataSourceInstanceID ds_id = data_source_instances_[name].id;
  EXPECT_CALL(*this, TearDownDataSourceInstance(ds_id))
      .WillOnce(InvokeWithoutArgs(on_ds_stop));
  task_runner_->RunUntilCheckpoint(checkpoint_name);
  data_source_instances_.erase(name);
}

std::unique_ptr<TraceWriter> MockProducer::CreateTraceWriter(
    const std::string& data_source_name) {
  PERFETTO_DCHECK(data_source_instances_.count(data_source_name));
  BufferID buf_id = data_source_instances_[data_source_name].target_buffer;
  return service_endpoint_->CreateTraceWriter(buf_id);
}

void MockProducer::WaitForFlush(TraceWriter* writer_to_flush) {
  auto& expected_call = EXPECT_CALL(*this, Flush(_, _, _));
  if (!writer_to_flush)
    return;
  expected_call.WillOnce(
      Invoke([this, writer_to_flush](FlushRequestID flush_req_id,
                                     const DataSourceInstanceID*, size_t) {
        writer_to_flush->Flush();
        service_endpoint_->NotifyFlushComplete(flush_req_id);
      }));
}

}  // namespace perfetto
