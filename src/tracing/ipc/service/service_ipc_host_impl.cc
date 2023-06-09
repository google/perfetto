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

#include "src/tracing/ipc/service/service_ipc_host_impl.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/ipc/host.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "src/tracing/ipc/service/consumer_ipc_service.h"
#include "src/tracing/ipc/service/producer_ipc_service.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include "src/tracing/ipc/shared_memory_windows.h"
#else
#include "src/tracing/ipc/posix_shared_memory.h"
#endif

namespace perfetto {

namespace {
constexpr uint32_t kProducerSocketTxTimeoutMs = 10;
}

// TODO(fmayer): implement per-uid connection limit (b/69093705).

// Implements the publicly exposed factory method declared in
// include/tracing/posix_ipc/posix_service_host.h.
std::unique_ptr<ServiceIPCHost> ServiceIPCHost::CreateInstance(
    base::TaskRunner* task_runner,
    TracingService::InitOpts init_opts) {
  return std::unique_ptr<ServiceIPCHost>(
      new ServiceIPCHostImpl(task_runner, init_opts));
}

ServiceIPCHostImpl::ServiceIPCHostImpl(base::TaskRunner* task_runner,
                                       TracingService::InitOpts init_opts)
    : task_runner_(task_runner), init_opts_(init_opts) {}

ServiceIPCHostImpl::~ServiceIPCHostImpl() {}

bool ServiceIPCHostImpl::Start(
    const std::vector<std::string>& producer_socket_names,
    const char* consumer_socket_name) {
  PERFETTO_CHECK(!svc_);  // Check if already started.

  // Initialize the IPC transport.
  for (const auto& producer_socket_name : producer_socket_names)
    producer_ipc_ports_.emplace_back(
        ipc::Host::CreateInstance(producer_socket_name.c_str(), task_runner_));
  consumer_ipc_port_ =
      ipc::Host::CreateInstance(consumer_socket_name, task_runner_);
  return DoStart();
}

bool ServiceIPCHostImpl::Start(base::ScopedSocketHandle producer_socket_fd,
                               base::ScopedSocketHandle consumer_socket_fd) {
  PERFETTO_CHECK(!svc_);  // Check if already started.

  // Initialize the IPC transport.
  producer_ipc_ports_.emplace_back(
      ipc::Host::CreateInstance(std::move(producer_socket_fd), task_runner_));
  consumer_ipc_port_ =
      ipc::Host::CreateInstance(std::move(consumer_socket_fd), task_runner_);
  return DoStart();
}

bool ServiceIPCHostImpl::Start(std::unique_ptr<ipc::Host> producer_host,
                               std::unique_ptr<ipc::Host> consumer_host) {
  PERFETTO_CHECK(!svc_);  // Check if already started.
  PERFETTO_DCHECK(producer_host);
  PERFETTO_DCHECK(consumer_host);

  // Initialize the IPC transport.
  producer_ipc_ports_.emplace_back(std::move(producer_host));
  consumer_ipc_port_ = std::move(consumer_host);

  return DoStart();
}

bool ServiceIPCHostImpl::DoStart() {
  // Create and initialize the platform-independent tracing business logic.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  std::unique_ptr<SharedMemory::Factory> shm_factory(
      new SharedMemoryWindows::Factory());
#else
  std::unique_ptr<SharedMemory::Factory> shm_factory(
      new PosixSharedMemory::Factory());
#endif
  svc_ = TracingService::CreateInstance(std::move(shm_factory), task_runner_,
                                        init_opts_);

  if (producer_ipc_ports_.empty() || !consumer_ipc_port_) {
    Shutdown();
    return false;
  }

  // Lower the timeout for blocking socket sends to producers as we shouldn't
  // normally exhaust the kernel send buffer unless the producer is
  // unresponsive. We'll drop the connection if the timeout is hit (see
  // UnixSocket::Send). Context in b/236813972, b/193234818.
  // Consumer port continues using the default timeout (10s) as there are
  // generally fewer consumer processes, and they're better behaved. Also the
  // consumer port ipcs might exhaust the send buffer under normal operation
  // due to large messages such as ReadBuffersResponse.
  for (auto& producer_ipc_port : producer_ipc_ports_)
    producer_ipc_port->SetSocketSendTimeoutMs(kProducerSocketTxTimeoutMs);

  // TODO(fmayer): add a test that destroyes the ServiceIPCHostImpl soon after
  // Start() and checks that no spurious callbacks are issued.
  for (auto& producer_ipc_port : producer_ipc_ports_) {
    bool producer_service_exposed = producer_ipc_port->ExposeService(
        std::unique_ptr<ipc::Service>(new ProducerIPCService(svc_.get())));
    PERFETTO_CHECK(producer_service_exposed);
  }

  bool consumer_service_exposed = consumer_ipc_port_->ExposeService(
      std::unique_ptr<ipc::Service>(new ConsumerIPCService(svc_.get())));
  PERFETTO_CHECK(consumer_service_exposed);

  return true;
}

TracingService* ServiceIPCHostImpl::service() const {
  return svc_.get();
}

void ServiceIPCHostImpl::Shutdown() {
  // TODO(primiano): add a test that causes the Shutdown() and checks that no
  // spurious callbacks are issued.
  producer_ipc_ports_.clear();
  consumer_ipc_port_.reset();
  svc_.reset();
}

// Definitions for the base class ctor/dtor.
ServiceIPCHost::ServiceIPCHost() = default;
ServiceIPCHost::~ServiceIPCHost() = default;

}  // namespace perfetto
