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

// TODO(fmayer): Add a test to check to what happens when ConsumerIPCClientImpl
// gets destroyed w.r.t. the Consumer pointer. Also think to lifetime of the
// Consumer* during the callbacks.

namespace perfetto {

// static. (Declared in include/tracing/ipc/consumer_ipc_client.h).
std::unique_ptr<TracingService::ConsumerEndpoint> ConsumerIPCClient::Connect(
    const char* service_sock_name,
    Consumer* consumer,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<TracingService::ConsumerEndpoint>(
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

void ConsumerIPCClientImpl::EnableTracing(const TraceConfig& trace_config,
                                          base::ScopedFile fd) {
  if (!connected_) {
    PERFETTO_DLOG("Cannot EnableTracing(), not connected to tracing service");
    return;
  }

  protos::EnableTracingRequest req;
  trace_config.ToProto(req.mutable_trace_config());
  ipc::Deferred<protos::EnableTracingResponse> async_response;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  async_response.Bind(
      [weak_this](ipc::AsyncResult<protos::EnableTracingResponse> response) {
        if (!weak_this)
          return;
        if (!response || response->disabled())
          weak_this->consumer_->OnTracingDisabled();
      });

  // |fd| will be closed when this function returns, but it's fine because the
  // IPC layer dup()'s it when sending the IPC.
  consumer_port_.EnableTracing(req, std::move(async_response), *fd);
}

void ConsumerIPCClientImpl::DisableTracing() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot DisableTracing(), not connected to tracing service");
    return;
  }

  ipc::Deferred<protos::DisableTracingResponse> async_response;
  async_response.Bind(
      [](ipc::AsyncResult<protos::DisableTracingResponse> response) {
        if (!response)
          PERFETTO_DLOG("DisableTracing() failed");
      });
  consumer_port_.DisableTracing(protos::DisableTracingRequest(),
                                std::move(async_response));
}

void ConsumerIPCClientImpl::ReadBuffers() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot ReadBuffers(), not connected to tracing service");
    return;
  }

  ipc::Deferred<protos::ReadBuffersResponse> async_response;

  // The IPC layer guarantees that callbacks are destroyed after this object
  // is destroyed (by virtue of destroying the |consumer_port_|). In turn the
  // contract of this class expects the caller to not destroy the Consumer class
  // before having destroyed this class. Hence binding |this| here is safe.
  async_response.Bind(
      [this](ipc::AsyncResult<protos::ReadBuffersResponse> response) {
        OnReadBuffersResponse(std::move(response));
      });
  consumer_port_.ReadBuffers(protos::ReadBuffersRequest(),
                             std::move(async_response));
}

void ConsumerIPCClientImpl::OnReadBuffersResponse(
    ipc::AsyncResult<protos::ReadBuffersResponse> response) {
  if (!response) {
    PERFETTO_DLOG("ReadBuffers() failed");
    return;
  }
  std::vector<TracePacket> trace_packets;
  for (auto& resp_slice : *response->mutable_slices()) {
    partial_packet_.AddSlice(
        Slice(std::unique_ptr<std::string>(resp_slice.release_data())));
    if (resp_slice.last_slice_for_packet())
      trace_packets.emplace_back(std::move(partial_packet_));
  }
  if (!trace_packets.empty() || !response.has_more())
    consumer_->OnTraceData(std::move(trace_packets), response.has_more());
}

void ConsumerIPCClientImpl::FreeBuffers() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot FreeBuffers(), not connected to tracing service");
    return;
  }

  protos::FreeBuffersRequest req;
  ipc::Deferred<protos::FreeBuffersResponse> async_response;
  async_response.Bind(
      [](ipc::AsyncResult<protos::FreeBuffersResponse> response) {
        if (!response)
          PERFETTO_DLOG("FreeBuffers() failed");
      });
  consumer_port_.FreeBuffers(req, std::move(async_response));
}

void ConsumerIPCClientImpl::Flush(uint32_t timeout_ms, FlushCallback callback) {
  if (!connected_) {
    PERFETTO_DLOG("Cannot Flush(), not connected to tracing service");
    return callback(/*success=*/false);
  }

  protos::FlushRequest req;
  req.set_timeout_ms(static_cast<uint32_t>(timeout_ms));
  ipc::Deferred<protos::FlushResponse> async_response;
  async_response.Bind(
      [callback](ipc::AsyncResult<protos::FlushResponse> response) {
        callback(!!response);
      });
  consumer_port_.Flush(req, std::move(async_response));
}

}  // namespace perfetto
