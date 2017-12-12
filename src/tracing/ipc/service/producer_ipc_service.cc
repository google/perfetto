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

#include "src/tracing/ipc/service/producer_ipc_service.h"

#include <inttypes.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ipc/host.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/service.h"
#include "src/tracing/ipc/posix_shared_memory.h"

// The remote Producer(s) are not trusted. All the methods from the ProducerPort
// IPC layer (e.g. RegisterDataSource()) must assume that the remote Producer is
// compromised.

namespace perfetto {

ProducerIPCService::ProducerIPCService(Service* core_service)
    : core_service_(core_service), weak_ptr_factory_(this) {}

ProducerIPCService::~ProducerIPCService() = default;

ProducerIPCService::RemoteProducer*
ProducerIPCService::GetProducerForCurrentRequest() {
  const ipc::ClientID ipc_client_id = ipc::Service::client_info().client_id();
  PERFETTO_CHECK(ipc_client_id);
  auto it = producers_.find(ipc_client_id);
  if (it == producers_.end())
    return nullptr;
  return it->second.get();
}

// Called by the remote Producer through the IPC channel soon after connecting.
void ProducerIPCService::InitializeConnection(
    const InitializeConnectionRequest& req,
    DeferredInitializeConnectionResponse response) {
  const ipc::ClientID ipc_client_id = ipc::Service::client_info().client_id();
  PERFETTO_CHECK(ipc_client_id);

  if (producers_.count(ipc_client_id) > 0) {
    PERFETTO_DLOG(
        "The remote Producer is trying to re-initialize the connection");
    return response.Reject();
  }

  // Create a new entry.
  std::unique_ptr<RemoteProducer> producer(new RemoteProducer());

  // ConnectProducer will call OnConnect() on the next task.
  producer->service_endpoint = core_service_->ConnectProducer(
      producer.get(), req.shared_buffer_size_hint_bytes());
  const int shm_fd = static_cast<PosixSharedMemory*>(
                         producer->service_endpoint->shared_memory())
                         ->fd();
  producers_.emplace(ipc_client_id, std::move(producer));
  // Because of the std::move() |producer| is invalid after this point.

  auto async_res = ipc::AsyncResult<InitializeConnectionResponse>::Create();
  async_res.set_fd(shm_fd);
  response.Resolve(std::move(async_res));
}

// Called by the remote Producer through the IPC channel.
void ProducerIPCService::RegisterDataSource(
    const RegisterDataSourceRequest& req,
    DeferredRegisterDataSourceResponse response) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked RegisterDataSource() before InitializeConnection()");
    return response.Reject();
  }

  const std::string data_source_name = req.data_source_descriptor().name();
  if (producer->pending_data_sources.count(data_source_name)) {
    PERFETTO_DLOG(
        "A RegisterDataSource() request for \"%s\" is already pending",
        data_source_name.c_str());
    return response.Reject();
  }

  // Deserialize IPC proto -> core DataSourceDescriptor. Keep this in sync with
  // changes to data_source_descriptor.proto.
  DataSourceDescriptor dsd;
  dsd.set_name(data_source_name);
  producer->pending_data_sources[data_source_name] = std::move(response);
  auto weak_this = weak_ptr_factory_.GetWeakPtr();

  // TODO: add test to cover the case of IPC going away before the
  // RegisterDataSource callback is received.
  const ipc::ClientID ipc_client_id = ipc::Service::client_info().client_id();
  GetProducerForCurrentRequest()->service_endpoint->RegisterDataSource(
      dsd, [weak_this, ipc_client_id, data_source_name](DataSourceID id) {
        if (!weak_this)
          return;
        weak_this->OnDataSourceRegistered(ipc_client_id, data_source_name, id);
      });
}

// Called by the Service business logic.
void ProducerIPCService::OnDataSourceRegistered(ipc::ClientID ipc_client_id,
                                                std::string data_source_name,
                                                DataSourceID id) {
  auto producer_it = producers_.find(ipc_client_id);
  if (producer_it == producers_.end())
    return;  // The producer died in the meantime.
  RemoteProducer* producer = producer_it->second.get();

  auto it = producer->pending_data_sources.find(data_source_name);
  PERFETTO_CHECK(it != producer->pending_data_sources.end());

  PERFETTO_DLOG("Data source %s registered, Client:%" PRIu64 " ID: %" PRIu64,
                data_source_name.c_str(), ipc_client_id, id);

  DeferredRegisterDataSourceResponse ipc_response = std::move(it->second);
  producer->pending_data_sources.erase(it);
  auto response = ipc::AsyncResult<RegisterDataSourceResponse>::Create();
  response->set_data_source_id(id);
  ipc_response.Resolve(std::move(response));
}

// Called by the IPC layer.
void ProducerIPCService::OnClientDisconnected() {
  ipc::ClientID client_id = ipc::Service::client_info().client_id();
  PERFETTO_DLOG("Client %" PRIu64 " disconnected", client_id);
  producers_.erase(client_id);
}

// TODO: test what happens if we receive the following tasks, in order:
// RegisterDataSource, UnregisterDataSource, OnDataSourceRegistered.
// which essentially means that the client posted back to back a
// ReqisterDataSource and UnregisterDataSource speculating on the next id.
// Called by the remote Service through the IPC channel.
void ProducerIPCService::UnregisterDataSource(
    const UnregisterDataSourceRequest& req,
    DeferredUnregisterDataSourceResponse response) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked UnregisterDataSource() before "
        "InitializeConnection()");
    return response.Reject();
  }
  producer->service_endpoint->UnregisterDataSource(req.data_source_id());

  // UnregisterDataSource doesn't expect any meaningful response.
  response.Resolve(ipc::AsyncResult<UnregisterDataSourceResponse>::Create());
}

void ProducerIPCService::NotifySharedMemoryUpdate(
    const NotifySharedMemoryUpdateRequest& req,
    DeferredNotifySharedMemoryUpdateResponse response) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked NotifySharedMemoryUpdate() before "
        "InitializeConnection()");
    return response.Reject();
  }
  // TODO: check that the page indexes are consistent with the size of the
  // shared memory region (once the SHM logic is there). Also add a test for it.
  std::vector<uint32_t> changed_pages;
  changed_pages.reserve(req.changed_pages_size());
  for (const uint32_t& changed_page : req.changed_pages())
    changed_pages.push_back(changed_page);
  producer->service_endpoint->NotifySharedMemoryUpdate(changed_pages);
  response.Resolve(
      ipc::AsyncResult<NotifySharedMemoryUpdateResponse>::Create());
}

void ProducerIPCService::GetAsyncCommand(
    const GetAsyncCommandRequest&,
    DeferredGetAsyncCommandResponse response) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked GetAsyncCommand() before "
        "InitializeConnection()");
    return response.Reject();
  }
  // Keep the back channel open, without ever resolving the ipc::Deferred fully,
  // to send async commands to the RemoteProducer (e.g., starting/stopping a
  // data source).
  producer->async_producer_commands = std::move(response);
}

////////////////////////////////////////////////////////////////////////////////
// RemoteProducer methods
////////////////////////////////////////////////////////////////////////////////

ProducerIPCService::RemoteProducer::RemoteProducer() = default;
ProducerIPCService::RemoteProducer::~RemoteProducer() = default;

// Invoked by the |core_service_| business logic after the ConnectProducer()
// call. There is nothing to do here, we really expected the ConnectProducer()
// to just work in the local case.
void ProducerIPCService::RemoteProducer::OnConnect() {}

// Invoked by the |core_service_| business logic after we destroy the
// |service_endpoint| (in the RemoteProducer dtor).
void ProducerIPCService::RemoteProducer::OnDisconnect() {}

// Invoked by the |core_service_| business logic when it wants to start a new
// data source.
void ProducerIPCService::RemoteProducer::CreateDataSourceInstance(
    DataSourceInstanceID dsid,
    const DataSourceConfig& cfg) {
  if (!async_producer_commands.IsBound()) {
    PERFETTO_DLOG(
        "The Service tried to start a new data source but the remote Producer "
        "has not yet initialized the connection");
    return;
  }
  auto cmd = ipc::AsyncResult<GetAsyncCommandResponse>::Create();
  cmd.set_has_more(true);
  cmd->mutable_start_data_source()->set_new_instance_id(dsid);

  // Keep this in sync with data_source_config.proto.
  cmd->mutable_start_data_source()
      ->mutable_config()
      ->set_trace_category_filters(cfg.trace_category_filters());
  async_producer_commands.Resolve(std::move(cmd));
}

void ProducerIPCService::RemoteProducer::TearDownDataSourceInstance(
    DataSourceInstanceID dsid) {
  if (!async_producer_commands.IsBound()) {
    PERFETTO_DLOG(
        "The Service tried to stop a data source but the remote Producer "
        "has not yet initialized the connection");
    return;
  }
  auto cmd = ipc::AsyncResult<GetAsyncCommandResponse>::Create();
  cmd.set_has_more(true);
  cmd->mutable_stop_data_source()->set_instance_id(dsid);
  async_producer_commands.Resolve(std::move(cmd));
}

}  // namespace perfetto
