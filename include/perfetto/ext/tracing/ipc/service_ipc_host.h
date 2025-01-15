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

#ifndef INCLUDE_PERFETTO_EXT_TRACING_IPC_SERVICE_IPC_HOST_H_
#define INCLUDE_PERFETTO_EXT_TRACING_IPC_SERVICE_IPC_HOST_H_

#include <memory>

#include "perfetto/base/export.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/default_socket.h"

namespace perfetto {
namespace base {
class TaskRunner;
}  // namespace base.

namespace ipc {
class Host;
}  // namespace ipc

// Creates an instance of the service (business logic + UNIX socket transport).
// Exposed to:
//   The code in the tracing client that will host the service e.g., traced.
// Implemented in:
//   src/tracing/ipc/service/service_ipc_host_impl.cc
class PERFETTO_EXPORT_COMPONENT ServiceIPCHost {
 public:
  static std::unique_ptr<ServiceIPCHost> CreateInstance(
      base::TaskRunner*,
      TracingService::InitOpts = {});
  virtual ~ServiceIPCHost();

  // The overload to wrap the multi-value producer socket name in the
  // single-value variant for compatibility in tests.
  // The socket name can be fd://123 to pass a pre-bound socket. This is used
  // when building as part of the Android tree, where init opens and binds the
  // socket beore exec()-ing us.
  bool Start(const char* producer_socket_name,
             const char* consumer_socket_name) {
    return Start(TokenizeProducerSockets(producer_socket_name),
                 consumer_socket_name);
  }

  // Start listening on the Producer & Consumer ports. Returns false in case of
  // failure (e.g., something else is listening on |socket_name|).
  virtual bool Start(const std::vector<std::string>& producer_socket_names,
                     const char* consumer_socket_name) = 0;

  // Allows callers to supply preconstructed Hosts.
  virtual bool Start(std::unique_ptr<ipc::Host> producer_host,
                     std::unique_ptr<ipc::Host> consumer_host) = 0;

  virtual TracingService* service() const = 0;

 protected:
  ServiceIPCHost();

 private:
  ServiceIPCHost(const ServiceIPCHost&) = delete;
  ServiceIPCHost& operator=(const ServiceIPCHost&) = delete;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_TRACING_IPC_SERVICE_IPC_HOST_H_
