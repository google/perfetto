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

#include "src/tracing/ipc/consumer/consumer_ipc_client_impl.h"

#include <inttypes.h>
#include <string.h>

#include "perfetto/base/task_runner.h"
#include "perfetto/ipc/client.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"

// TODO Add a test to check to what happens when ConsumerIPCClientImpl gets
// destroyed w.r.t. the Consumer pointer. Also think to lifetime of the
// Consumer* during the callbacks.

namespace perfetto {

// static. (Declared in include/tracing/ipc/consumer_ipc_client.h).
std::unique_ptr<Service::ConsumerEndpoint> ConsumerIPCClient::Connect(
    const char* service_sock_name,
    Consumer* consumer,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<Service::ConsumerEndpoint>(
      new ConsumerIPCClientImpl(service_sock_name, consumer, task_runner));
}

ConsumerIPCClientImpl::ConsumerIPCClientImpl(const char* service_sock_name,
                                             Consumer* consumer,
                                             base::TaskRunner* task_runner)
    : consumer_(consumer),
      ipc_channel_(ipc::Client::CreateInstance(service_sock_name, task_runner)),
      consumer_port_(this /* event_listener */),
      weak_ptr_factory_(this) {
  ipc_channel_->BindService(consumer_port_.GetWeakPtr());
}

ConsumerIPCClientImpl::~ConsumerIPCClientImpl() = default;

// Called by the IPC layer if the BindService() succeeds.
void ConsumerIPCClientImpl::OnConnect() {
  connected_ = true;
  consumer_->OnConnect();
}

void ConsumerIPCClientImpl::OnDisconnect() {
  PERFETTO_DLOG("Tracing service connection failure");
  connected_ = false;
  consumer_->OnDisconnect();
}

void ConsumerIPCClientImpl::EnableTracing(const TraceConfig& trace_config) {
  if (!connected_) {
    PERFETTO_DLOG("Cannot EnableTracing(), not connected to tracing service");
    return;
  }

  // Serialize the |trace_config| into a EnableTracingRequest protobuf.
  // Keep this in sync with changes in consumer_port.proto.
  EnableTracingRequest req;
  trace_config.ToProto(req.mutable_trace_config());
  ipc::Deferred<EnableTracingResponse> async_response;
  async_response.Bind([](ipc::AsyncResult<EnableTracingResponse> response) {
    if (!response)
      PERFETTO_DLOG("EnableTracing() failed");
  });
  consumer_port_.EnableTracing(req, std::move(async_response));
}

void ConsumerIPCClientImpl::DisableTracing() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot DisableTracing(), not connected to tracing service");
    return;
  }

  ipc::Deferred<DisableTracingResponse> async_response;
  async_response.Bind([](ipc::AsyncResult<DisableTracingResponse> response) {
    if (!response)
      PERFETTO_DLOG("DisableTracing() failed");
  });
  consumer_port_.DisableTracing(DisableTracingRequest(),
                                std::move(async_response));
}

void ConsumerIPCClientImpl::ReadBuffers() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot ReadBuffers(), not connected to tracing service");
    return;
  }

  ipc::Deferred<ReadBuffersResponse> async_response;

  // The IPC layer guarantees that callbacks are destroyed after this object
  // is destroyed (by virtue of destroying the |consumer_port_|). In turn the
  // contract of this class expects the caller to not destroy the Consumer class
  // before having destroyed this class. Hence binding |this| here is safe.
  async_response.Bind([this](ipc::AsyncResult<ReadBuffersResponse> response) {
    OnReadBuffersResponse(std::move(response));
  });
  consumer_port_.ReadBuffers(ReadBuffersRequest(), std::move(async_response));
}

void ConsumerIPCClientImpl::OnReadBuffersResponse(
    ipc::AsyncResult<ReadBuffersResponse> response) {
  if (!response) {
    PERFETTO_DLOG("ReadBuffers() failed");
    return;
  }
  // TODO(primiano): We have to guarantee that the log buffer stays alive at
  // least as long as these requests are on flights.
  std::vector<TracePacket> trace_packets;
  trace_packets.reserve(response->trace_packets().size());
  for (const std::string& bytes : response->trace_packets()) {
    trace_packets.emplace_back();
    trace_packets.back().AddChunk(
        Chunk(reinterpret_cast<const void*>(bytes.data()), bytes.size()));
  }
  consumer_->OnTraceData(trace_packets, response.has_more());
}

void ConsumerIPCClientImpl::FreeBuffers() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot FreeBuffers(), not connected to tracing service");
    return;
  }

  FreeBuffersRequest req;
  ipc::Deferred<FreeBuffersResponse> async_response;
  async_response.Bind([](ipc::AsyncResult<FreeBuffersResponse> response) {
    if (!response)
      PERFETTO_DLOG("FreeBuffers() failed");
  });
  consumer_port_.FreeBuffers(req, std::move(async_response));
}

}  // namespace perfetto
