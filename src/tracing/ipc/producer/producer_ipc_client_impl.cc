/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/ipc/producer/producer_ipc_client_impl.h"

#include <inttypes.h>
#include <string.h>

#include "perfetto/base/task_runner.h"
#include "perfetto/ipc/client.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory_arbiter.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/tracing/ipc/posix_shared_memory.h"

// TODO(fmayer): think to what happens when ProducerIPCClientImpl gets destroyed
// w.r.t. the Producer pointer. Also think to lifetime of the Producer* during
// the callbacks.

namespace perfetto {

// static. (Declared in include/tracing/ipc/producer_ipc_client.h).
std::unique_ptr<TracingService::ProducerEndpoint> ProducerIPCClient::Connect(
    const char* service_sock_name,
    Producer* producer,
    const std::string& producer_name,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<TracingService::ProducerEndpoint>(
      new ProducerIPCClientImpl(service_sock_name, producer, producer_name,
                                task_runner));
}

ProducerIPCClientImpl::ProducerIPCClientImpl(const char* service_sock_name,
                                             Producer* producer,
                                             const std::string& producer_name,
                                             base::TaskRunner* task_runner)
    : producer_(producer),
      task_runner_(task_runner),
      ipc_channel_(ipc::Client::CreateInstance(service_sock_name, task_runner)),
      producer_port_(this /* event_listener */),
      name_(producer_name) {
  ipc_channel_->BindService(producer_port_.GetWeakPtr());
  PERFETTO_DCHECK_THREAD(thread_checker_);
}

ProducerIPCClientImpl::~ProducerIPCClientImpl() = default;

// Called by the IPC layer if the BindService() succeeds.
void ProducerIPCClientImpl::OnConnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  connected_ = true;

  // The IPC layer guarantees that any outstanding callback will be dropped on
  // the floor if producer_port_ is destroyed between the request and the reply.
  // Binding |this| is hence safe.
  ipc::Deferred<protos::InitializeConnectionResponse> on_init;
  on_init.Bind(
      [this](ipc::AsyncResult<protos::InitializeConnectionResponse> resp) {
        OnConnectionInitialized(resp.success());
      });
  protos::InitializeConnectionRequest req;
  req.set_producer_name(name_);
  producer_port_.InitializeConnection(req, std::move(on_init));

  // Create the back channel to receive commands from the Service.
  ipc::Deferred<protos::GetAsyncCommandResponse> on_cmd;
  on_cmd.Bind([this](ipc::AsyncResult<protos::GetAsyncCommandResponse> resp) {
    if (!resp)
      return;  // The IPC channel was closed and |resp| was auto-rejected.
    OnServiceRequest(*resp);
  });
  producer_port_.GetAsyncCommand(protos::GetAsyncCommandRequest(),
                                 std::move(on_cmd));
}

void ProducerIPCClientImpl::OnDisconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Tracing service connection failure");
  connected_ = false;
  producer_->OnDisconnect();
}

void ProducerIPCClientImpl::OnConnectionInitialized(bool connection_succeeded) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // If connection_succeeded == false, the OnDisconnect() call will follow next
  // and there we'll notify the |producer_|. TODO: add a test for this.
  if (!connection_succeeded)
    return;
  producer_->OnConnect();
}

void ProducerIPCClientImpl::OnServiceRequest(
    const protos::GetAsyncCommandResponse& cmd) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (cmd.cmd_case() == protos::GetAsyncCommandResponse::kStartDataSource) {
    const auto& req = cmd.start_data_source();
    const DataSourceInstanceID dsid = req.new_instance_id();
    DataSourceConfig cfg;
    cfg.FromProto(req.config());
    producer_->CreateDataSourceInstance(dsid, cfg);
    return;
  }

  if (cmd.cmd_case() == protos::GetAsyncCommandResponse::kStopDataSource) {
    const DataSourceInstanceID dsid = cmd.stop_data_source().instance_id();
    producer_->TearDownDataSourceInstance(dsid);
    return;
  }

  if (cmd.cmd_case() == protos::GetAsyncCommandResponse::kSetupTracing) {
    base::ScopedFile shmem_fd = ipc_channel_->TakeReceivedFD();
    PERFETTO_CHECK(shmem_fd);

    // TODO(primiano): handle mmap failure in case of OOM.
    shared_memory_ = PosixSharedMemory::AttachToFd(std::move(shmem_fd));
    shared_buffer_page_size_kb_ =
        cmd.setup_tracing().shared_buffer_page_size_kb();
    shared_memory_arbiter_ = SharedMemoryArbiter::CreateInstance(
        shared_memory_.get(), shared_buffer_page_size_kb_ * 1024, this,
        task_runner_);
    producer_->OnTracingSetup();
    return;
  }

  if (cmd.cmd_case() == protos::GetAsyncCommandResponse::kFlush) {
    // This cast boilerplate is required only because protobuf uses its own
    // uint64 and not stdint's uint64_t. On some 64 bit archs they differ on the
    // type (long vs long long) even though they have the same size.
    const auto* data_source_ids = cmd.flush().data_source_ids().data();
    static_assert(sizeof(data_source_ids[0]) == sizeof(FlushRequestID),
                  "data_source_ids should be 64-bit");
    producer_->Flush(cmd.flush().request_id(),
                     reinterpret_cast<const FlushRequestID*>(data_source_ids),
                     static_cast<size_t>(cmd.flush().data_source_ids().size()));
    return;
  }

  PERFETTO_DLOG("Unknown async request %d received from tracing service",
                cmd.cmd_case());
  PERFETTO_DCHECK(false);
}

void ProducerIPCClientImpl::RegisterDataSource(
    const DataSourceDescriptor& descriptor) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot RegisterDataSource(), not connected to tracing service");
  }
  protos::RegisterDataSourceRequest req;
  descriptor.ToProto(req.mutable_data_source_descriptor());
  ipc::Deferred<protos::RegisterDataSourceResponse> async_response;
  async_response.Bind(
      [](ipc::AsyncResult<protos::RegisterDataSourceResponse> response) {
        if (!response)
          PERFETTO_DLOG("RegisterDataSource() failed: connection reset");
      });
  producer_port_.RegisterDataSource(req, std::move(async_response));
}

void ProducerIPCClientImpl::UnregisterDataSource(const std::string& name) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot UnregisterDataSource(), not connected to tracing service");
    return;
  }
  protos::UnregisterDataSourceRequest req;
  req.set_data_source_name(name);
  producer_port_.UnregisterDataSource(
      req, ipc::Deferred<protos::UnregisterDataSourceResponse>());
}

void ProducerIPCClientImpl::CommitData(const CommitDataRequest& req,
                                       CommitDataCallback callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!connected_) {
    PERFETTO_DLOG("Cannot CommitData(), not connected to tracing service");
    return;
  }
  protos::CommitDataRequest proto_req;
  req.ToProto(&proto_req);
  ipc::Deferred<protos::CommitDataResponse> async_response;
  // TODO(primiano): add a test that destroys ProducerIPCClientImpl soon after
  // this call and checks that the callback is dropped.
  if (callback) {
    async_response.Bind(
        [callback](ipc::AsyncResult<protos::CommitDataResponse> response) {
          if (!response) {
            PERFETTO_DLOG("CommitData() failed: connection reset");
            return;
          }
          callback();
        });
  }
  producer_port_.CommitData(proto_req, std::move(async_response));
}

std::unique_ptr<TraceWriter> ProducerIPCClientImpl::CreateTraceWriter(
    BufferID target_buffer) {
  // This method can be called by different threads. |shared_memory_arbiter_| is
  // thread-safe but be aware of accessing any other state in this function.
  return shared_memory_arbiter_->CreateTraceWriter(target_buffer);
}

void ProducerIPCClientImpl::NotifyFlushComplete(FlushRequestID req_id) {
  return shared_memory_arbiter_->NotifyFlushComplete(req_id);
}

SharedMemory* ProducerIPCClientImpl::shared_memory() const {
  return shared_memory_.get();
}

size_t ProducerIPCClientImpl::shared_buffer_page_size_kb() const {
  return shared_buffer_page_size_kb_;
}

}  // namespace perfetto
