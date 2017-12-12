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
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "src/tracing/ipc/posix_shared_memory.h"

// TODO think to what happens when ProducerIPCClientImpl gets destroyed
// w.r.t. the Producer pointer. Also think to lifetime of the Producer* during
// the callbacks.

namespace perfetto {

// static. (Declared in include/tracing/ipc/producer_ipc_client.h).
std::unique_ptr<Service::ProducerEndpoint> ProducerIPCClient::Connect(
    const char* service_sock_name,
    Producer* producer,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<Service::ProducerEndpoint>(
      new ProducerIPCClientImpl(service_sock_name, producer, task_runner));
}

ProducerIPCClientImpl::ProducerIPCClientImpl(const char* service_sock_name,
                                             Producer* producer,
                                             base::TaskRunner* task_runner)
    : producer_(producer),
      task_runner_(task_runner),
      ipc_channel_(ipc::Client::CreateInstance(service_sock_name, task_runner)),
      producer_port_(this /* event_listener */) {
  ipc_channel_->BindService(producer_port_.GetWeakPtr());
}

ProducerIPCClientImpl::~ProducerIPCClientImpl() = default;

// Called by the IPC layer if the BindService() succeeds.
void ProducerIPCClientImpl::OnConnect() {
  connected_ = true;

  // The IPC layer guarantees that any outstanding callback will be dropped on
  // the floor if producer_port_ is destroyed between the request and the reply.
  // Binding |this| is hence safe.
  ipc::Deferred<InitializeConnectionResponse> on_init;
  on_init.Bind([this](ipc::AsyncResult<InitializeConnectionResponse> resp) {
    OnConnectionInitialized(resp.success());
  });
  producer_port_.InitializeConnection(InitializeConnectionRequest(),
                                      std::move(on_init));

  // Create the back channel to receive commands from the Service.
  ipc::Deferred<GetAsyncCommandResponse> on_cmd;
  on_cmd.Bind([this](ipc::AsyncResult<GetAsyncCommandResponse> resp) {
    if (!resp)
      return;  // The IPC channel was closed and |resp| was auto-rejected.
    OnServiceRequest(*resp);
  });
  producer_port_.GetAsyncCommand(GetAsyncCommandRequest(), std::move(on_cmd));
}

void ProducerIPCClientImpl::OnDisconnect() {
  PERFETTO_DLOG("Tracing service connection failure");
  connected_ = false;
  producer_->OnDisconnect();
}

void ProducerIPCClientImpl::OnConnectionInitialized(bool connection_succeeded) {
  // If connection_succeeded == false, the OnDisconnect() call will follow next
  // and there we'll notify the |producer_|. TODO: add a test for this.
  if (!connection_succeeded)
    return;

  base::ScopedFile shmem_fd = ipc_channel_->TakeReceivedFD();
  PERFETTO_CHECK(shmem_fd);
  shared_memory_ = PosixSharedMemory::AttachToFd(std::move(shmem_fd));
  producer_->OnConnect();
}

void ProducerIPCClientImpl::OnServiceRequest(
    const GetAsyncCommandResponse& cmd) {
  if (cmd.cmd_case() == GetAsyncCommandResponse::kStartDataSource) {
    // Keep this in sync with chages in data_source_config.proto.
    const auto& req = cmd.start_data_source();
    const DataSourceInstanceID dsid = req.new_instance_id();
    const protos::DataSourceConfig& proto_cfg = req.config();
    DataSourceConfig cfg;
    cfg.set_trace_category_filters(proto_cfg.trace_category_filters());
    producer_->CreateDataSourceInstance(dsid, cfg);
    return;
  }

  if (cmd.cmd_case() == GetAsyncCommandResponse::kStopDataSource) {
    const DataSourceInstanceID dsid = cmd.stop_data_source().instance_id();
    producer_->TearDownDataSourceInstance(dsid);
    return;
  }

  PERFETTO_DLOG("Unknown async request %d received from tracing service",
                cmd.cmd_case());
}

void ProducerIPCClientImpl::RegisterDataSource(
    const DataSourceDescriptor& descriptor,
    RegisterDataSourceCallback callback) {
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot RegisterDataSource(), not connected to tracing service");
    return task_runner_->PostTask(std::bind(callback, 0));
  }
  // Keep this in sync with changes in data_source_descriptor.proto.
  RegisterDataSourceRequest req;
  auto* proto_descriptor = req.mutable_data_source_descriptor();
  proto_descriptor->set_name(descriptor.name());
  ipc::Deferred<RegisterDataSourceResponse> async_response;
  // TODO: add a test that destroys the IPC channel soon after this call and
  // checks that the callback(0) is invoked.
  // TODO: add a test that destroyes ProducerIPCClientImpl soon after this call
  // and checks that the callback is dropped.
  async_response.Bind(
      [callback](ipc::AsyncResult<RegisterDataSourceResponse> response) {
        if (!response) {
          PERFETTO_DLOG("RegisterDataSource() failed: connection reset");
          return callback(0);
        }
        if (response->data_source_id() == 0) {
          PERFETTO_DLOG("RegisterDataSource() failed: %s",
                        response->error().c_str());
        }
        callback(response->data_source_id());
      });
  producer_port_.RegisterDataSource(req, std::move(async_response));
}

void ProducerIPCClientImpl::UnregisterDataSource(DataSourceID id) {
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot UnregisterDataSource(), not connected to tracing service");
    return;
  }
  UnregisterDataSourceRequest req;
  req.set_data_source_id(id);
  producer_port_.UnregisterDataSource(
      req, ipc::Deferred<UnregisterDataSourceResponse>());
}

void ProducerIPCClientImpl::NotifySharedMemoryUpdate(
    const std::vector<uint32_t>& changed_pages) {
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot NotifySharedMemoryUpdate(), not connected to tracing service");
    return;
  }
  NotifySharedMemoryUpdateRequest req;
  for (uint32_t changed_page : changed_pages)
    req.add_changed_pages(changed_page);
  producer_port_.NotifySharedMemoryUpdate(
      req, ipc::Deferred<NotifySharedMemoryUpdateResponse>());
}

SharedMemory* ProducerIPCClientImpl::shared_memory() const {
  return shared_memory_.get();
}

}  // namespace perfetto
