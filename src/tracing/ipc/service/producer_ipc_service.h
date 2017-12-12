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

#ifndef SRC_TRACING_IPC_SERVICE_PRODUCER_IPC_SERVICE_H_
#define SRC_TRACING_IPC_SERVICE_PRODUCER_IPC_SERVICE_H_

#include <map>
#include <memory>
#include <string>

#include "perfetto/base/weak_ptr.h"
#include "perfetto/ipc/basic_types.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/service.h"

#include "protos/tracing_service/producer_port.ipc.h"

namespace perfetto {

namespace ipc {
class Host;
}  // namespace ipc

// Implements the Producer port of the IPC service. This class proxies requests
// and responses between the core service logic (|svc_|) and remote Producer(s)
// on the IPC socket, through the methods overriddden from ProducerPort.
class ProducerIPCService : public ProducerPort /* from producer_port.proto */ {
 public:
  using Service = ::perfetto::Service;  // To avoid collisions w/ ipc::Service.
  explicit ProducerIPCService(Service* core_service);
  ~ProducerIPCService() override;

  // ProducerPort implementation (from .proto IPC definition).
  void InitializeConnection(const InitializeConnectionRequest&,
                            DeferredInitializeConnectionResponse) override;
  void RegisterDataSource(const RegisterDataSourceRequest&,
                          DeferredRegisterDataSourceResponse) override;
  void UnregisterDataSource(const UnregisterDataSourceRequest&,
                            DeferredUnregisterDataSourceResponse) override;
  void NotifySharedMemoryUpdate(
      const NotifySharedMemoryUpdateRequest&,
      DeferredNotifySharedMemoryUpdateResponse) override;
  void GetAsyncCommand(const GetAsyncCommandRequest&,
                       DeferredGetAsyncCommandResponse) override;
  void OnClientDisconnected() override;

 private:
  // Acts like a Producer with the core Service business logic (which doesn't
  // know anything about the remote transport), but all it does is proxying
  // methods to the remote Producer on the other side of the IPC channel.
  class RemoteProducer : public Producer {
   public:
    RemoteProducer();
    ~RemoteProducer() override;

    // These methods are called by the |core_service_| business logic. There is
    // no connection here, these methods are posted straight away.
    void OnConnect() override;
    void OnDisconnect() override;
    void CreateDataSourceInstance(DataSourceInstanceID,
                                  const DataSourceConfig&) override;
    void TearDownDataSourceInstance(DataSourceInstanceID) override;

    // RegisterDataSource requests that haven't been replied yet.
    std::map<std::string, DeferredRegisterDataSourceResponse>
        pending_data_sources;

    // The interface obtained from the core service business logic through
    // Service::ConnectProducer(this). This allows to invoke methods for a
    // specific Producer on the Service business logic.
    std::unique_ptr<Service::ProducerEndpoint> service_endpoint;

    // The back-channel (based on a never ending stream request) that allows us
    // to send asynchronous commands to the remote Producer (e.g. start/stop a
    // data source).
    DeferredGetAsyncCommandResponse async_producer_commands;
  };

  ProducerIPCService(const ProducerIPCService&) = delete;
  ProducerIPCService& operator=(const ProducerIPCService&) = delete;

  // Returns the ProducerEndpoint in the core business logic that corresponds to
  // the current IPC request.
  RemoteProducer* GetProducerForCurrentRequest();

  // Called back by the |core_service_| business logic, soon after a call to
  // RegisterDataSource().
  void OnDataSourceRegistered(ipc::ClientID, std::string, DataSourceID);

  Service* const core_service_;
  base::WeakPtrFactory<ProducerIPCService> weak_ptr_factory_;

  // Maps IPC clients to ProducerEndpoint instances registered on the
  // |core_service_| business logic.
  std::map<ipc::ClientID, std::unique_ptr<RemoteProducer>> producers_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_IPC_SERVICE_PRODUCER_IPC_SERVICE_H_
