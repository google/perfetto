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
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/tracing_service.h"
#include "src/tracing/ipc/posix_shared_memory.h"

// The remote Producer(s) are not trusted. All the methods from the ProducerPort
// IPC layer (e.g. RegisterDataSource()) must assume that the remote Producer is
// compromised.

namespace perfetto {

ProducerIPCService::ProducerIPCService(TracingService* core_service)
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
    const protos::InitializeConnectionRequest& req,
    DeferredInitializeConnectionResponse response) {
  const auto& client_info = ipc::Service::client_info();
  const ipc::ClientID ipc_client_id = client_info.client_id();
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
      producer.get(), client_info.uid(), req.producer_name(),
      req.shared_memory_size_hint_bytes());

  // Could happen if the service has too many producers connected.
  if (!producer->service_endpoint)
    response.Reject();

  producers_.emplace(ipc_client_id, std::move(producer));
  // Because of the std::move() |producer| is invalid after this point.

  auto async_res =
      ipc::AsyncResult<protos::InitializeConnectionResponse>::Create();
  response.Resolve(std::move(async_res));
}

// Called by the remote Producer through the IPC channel.
void ProducerIPCService::RegisterDataSource(
    const protos::RegisterDataSourceRequest& req,
    DeferredRegisterDataSourceResponse response) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked RegisterDataSource() before InitializeConnection()");
    return response.Reject();
  }

  DataSourceDescriptor dsd;
  dsd.FromProto(req.data_source_descriptor());
  GetProducerForCurrentRequest()->service_endpoint->RegisterDataSource(dsd);

  // RegisterDataSource doesn't expect any meaningful response.
  response.Resolve(
      ipc::AsyncResult<protos::RegisterDataSourceResponse>::Create());
}

// Called by the IPC layer.
void ProducerIPCService::OnClientDisconnected() {
  ipc::ClientID client_id = ipc::Service::client_info().client_id();
  PERFETTO_DLOG("Client %" PRIu64 " disconnected", client_id);
  producers_.erase(client_id);
}

// TODO(fmayer): test what happens if we receive the following tasks, in order:
// RegisterDataSource, UnregisterDataSource, OnDataSourceRegistered.
// which essentially means that the client posted back to back a
// ReqisterDataSource and UnregisterDataSource speculating on the next id.
// Called by the remote Service through the IPC channel.
void ProducerIPCService::UnregisterDataSource(
    const protos::UnregisterDataSourceRequest& req,
    DeferredUnregisterDataSourceResponse response) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked UnregisterDataSource() before "
        "InitializeConnection()");
    return response.Reject();
  }
  producer->service_endpoint->UnregisterDataSource(req.data_source_name());

  // UnregisterDataSource doesn't expect any meaningful response.
  response.Resolve(
      ipc::AsyncResult<protos::UnregisterDataSourceResponse>::Create());
}

void ProducerIPCService::CommitData(const protos::CommitDataRequest& proto_req,
                                    DeferredCommitDataResponse resp) {
  RemoteProducer* producer = GetProducerForCurrentRequest();
  if (!producer) {
    PERFETTO_DLOG(
        "Producer invoked CommitData() before InitializeConnection()");
    return;
  }
  CommitDataRequest req;
  req.FromProto(proto_req);

  // We don't want to send a response if the client didn't attach a callback to
  // the original request. Doing so would generate unnecessary wakeups and
  // context switches.
  std::function<void()> callback;
  if (resp.IsBound()) {
    // Capturing |resp| by reference here speculates on the fact that
    // CommitData() in tracing_service_impl.cc invokes the passed callback
    // inline, without posting it. If that assumption changes this code needs to
    // wrap the response in a shared_ptr (C+11 lambdas don't support move) and
    // use a weak ptr in the caller.
    callback = [&resp] {
      resp.Resolve(ipc::AsyncResult<protos::CommitDataResponse>::Create());
    };
  }
  producer->service_endpoint->CommitData(req, callback);
}

void ProducerIPCService::GetAsyncCommand(
    const protos::GetAsyncCommandRequest&,
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
  auto cmd = ipc::AsyncResult<protos::GetAsyncCommandResponse>::Create();
  cmd.set_has_more(true);
  cmd->mutable_start_data_source()->set_new_instance_id(dsid);
  cfg.ToProto(cmd->mutable_start_data_source()->mutable_config());
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
  auto cmd = ipc::AsyncResult<protos::GetAsyncCommandResponse>::Create();
  cmd.set_has_more(true);
  cmd->mutable_stop_data_source()->set_instance_id(dsid);
  async_producer_commands.Resolve(std::move(cmd));
}

void ProducerIPCService::RemoteProducer::OnTracingSetup() {
  if (!async_producer_commands.IsBound()) {
    PERFETTO_DLOG(
        "The Service tried to allocate the shared memory but the remote "
        "Producer has not yet initialized the connection");
    return;
  }
  PERFETTO_CHECK(service_endpoint->shared_memory());
  const int shm_fd =
      static_cast<PosixSharedMemory*>(service_endpoint->shared_memory())->fd();
  auto cmd = ipc::AsyncResult<protos::GetAsyncCommandResponse>::Create();
  cmd.set_has_more(true);
  cmd.set_fd(shm_fd);
  cmd->mutable_setup_tracing()->set_shared_buffer_page_size_kb(
      static_cast<uint32_t>(service_endpoint->shared_buffer_page_size_kb()));
  async_producer_commands.Resolve(std::move(cmd));
}

void ProducerIPCService::RemoteProducer::Flush(
    FlushRequestID flush_request_id,
    const DataSourceInstanceID* data_source_ids,
    size_t num_data_sources) {
  if (!async_producer_commands.IsBound()) {
    PERFETTO_DLOG(
        "The Service tried to request a flush but the remote Producer has not "
        "yet initialized the connection");
    return;
  }
  auto cmd = ipc::AsyncResult<protos::GetAsyncCommandResponse>::Create();
  cmd.set_has_more(true);
  for (size_t i = 0; i < num_data_sources; i++)
    cmd->mutable_flush()->add_data_source_ids(data_source_ids[i]);
  cmd->mutable_flush()->set_request_id(flush_request_id);
  async_producer_commands.Resolve(std::move(cmd));
}

}  // namespace perfetto
