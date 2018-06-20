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

#ifndef INCLUDE_PERFETTO_TRACING_IPC_PRODUCER_IPC_CLIENT_H_
#define INCLUDE_PERFETTO_TRACING_IPC_PRODUCER_IPC_CLIENT_H_

#include <memory>
#include <string>

#include "perfetto/tracing/core/tracing_service.h"

namespace perfetto {

class Producer;

// Allows to connect to a remote Service through a UNIX domain socket.
// Exposed to:
//   Producer(s) of the tracing library.
// Implemented in:
//   src/tracing/ipc/producer/producer_ipc_client_impl.cc
class ProducerIPCClient {
 public:
  // Connects to the producer port of the Service listening on the given
  // |service_sock_name|. If the connection is successful, the OnConnect()
  // method will be invoked asynchronously on the passed Producer interface.
  // If the connection fails, OnDisconnect() will be invoked instead.
  // The returned ProducerEndpoint serves also to delimit the scope of the
  // callbacks invoked on the Producer interface: no more Producer callbacks are
  // invoked immediately after its destruction and any pending callback will be
  // dropped.
  static std::unique_ptr<TracingService::ProducerEndpoint> Connect(
      const char* service_sock_name,
      Producer*,
      const std::string& producer_name,
      base::TaskRunner*);

 protected:
  ProducerIPCClient() = delete;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_IPC_PRODUCER_IPC_CLIENT_H_
