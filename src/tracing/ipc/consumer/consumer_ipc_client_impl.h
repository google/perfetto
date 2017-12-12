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

#ifndef SRC_TRACING_IPC_CONSUMER_CONSUMER_IPC_CLIENT_IMPL_H_
#define SRC_TRACING_IPC_CONSUMER_CONSUMER_IPC_CLIENT_IMPL_H_

#include <stdint.h>

#include <vector>

#include "perfetto/base/weak_ptr.h"
#include "perfetto/ipc/service_proxy.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/service.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"

#include "protos/tracing_service/consumer_port.ipc.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

namespace ipc {
class Client;
}  // namespace ipc

class Consumer;
class TraceConfig;

// Exposes a Service endpoint to Consumer(s), proxying all requests through a
// IPC channel to the remote Service. This class is the glue layer between the
// generic Service interface exposed to the clients of the library and the
// actual IPC transport.
class ConsumerIPCClientImpl : public Service::ConsumerEndpoint,
                              public ipc::ServiceProxy::EventListener {
 public:
  ConsumerIPCClientImpl(const char* service_sock_name,
                        Consumer*,
                        base::TaskRunner*);
  ~ConsumerIPCClientImpl() override;

  // Service::ConsumerEndpoint implementation.
  // These methods are invoked by the actual Consumer(s) code by clients of the
  // tracing library, which know nothing about the IPC transport.
  void EnableTracing(const TraceConfig&) override;
  void DisableTracing() override;
  void ReadBuffers() override;
  void FreeBuffers() override;

  // ipc::ServiceProxy::EventListener implementation.
  // These methods are invoked by the IPC layer, which knows nothing about
  // tracing, consumers and consumers.
  void OnConnect() override;
  void OnDisconnect() override;

 private:
  void OnReadBuffersResponse(ipc::AsyncResult<ReadBuffersResponse>);

  // TODO(primiano): think to dtor order, do we rely on any specific sequence?
  Consumer* const consumer_;

  // The object that owns the client socket and takes care of IPC traffic.
  std::unique_ptr<ipc::Client> ipc_channel_;

  // The proxy interface for the consumer port of the service. It is bound
  // to |ipc_channel_| and (de)serializes method invocations over the wire.
  ConsumerPortProxy consumer_port_;

  base::WeakPtrFactory<ConsumerIPCClientImpl> weak_ptr_factory_;

  bool connected_ = false;
};

}  // namespace perfetto

#endif  // SRC_TRACING_IPC_CONSUMER_CONSUMER_IPC_CLIENT_IMPL_H_
