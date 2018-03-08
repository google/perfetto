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

#include "src/tracing/ipc/service/consumer_ipc_service.h"

#include <inttypes.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ipc/host.h"
#include "perfetto/tracing/core/service.h"
#include "perfetto/tracing/core/slice.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"

namespace perfetto {

ConsumerIPCService::ConsumerIPCService(Service* core_service)
    : core_service_(core_service), weak_ptr_factory_(this) {}

ConsumerIPCService::~ConsumerIPCService() = default;

ConsumerIPCService::RemoteConsumer*
ConsumerIPCService::GetConsumerForCurrentRequest() {
  const ipc::ClientID ipc_client_id = ipc::Service::client_info().client_id();
  PERFETTO_CHECK(ipc_client_id);
  auto it = consumers_.find(ipc_client_id);
  if (it == consumers_.end()) {
    auto* remote_consumer = new RemoteConsumer();
    consumers_[ipc_client_id].reset(remote_consumer);
    remote_consumer->service_endpoint =
        core_service_->ConnectConsumer(remote_consumer);
    return remote_consumer;
  }
  return it->second.get();
}

// Called by the IPC layer.
void ConsumerIPCService::OnClientDisconnected() {
  ipc::ClientID client_id = ipc::Service::client_info().client_id();
  consumers_.erase(client_id);
}

// Called by the IPC layer.
void ConsumerIPCService::EnableTracing(const protos::EnableTracingRequest& req,
                                       DeferredEnableTracingResponse resp) {
  TraceConfig trace_config;
  trace_config.FromProto(req.trace_config());
  GetConsumerForCurrentRequest()->service_endpoint->EnableTracing(trace_config);
  resp.Resolve(ipc::AsyncResult<protos::EnableTracingResponse>::Create());
}

// Called by the IPC layer.
void ConsumerIPCService::DisableTracing(
    const protos::DisableTracingRequest& req,
    DeferredDisableTracingResponse resp) {
  GetConsumerForCurrentRequest()->service_endpoint->DisableTracing();
  resp.Resolve(ipc::AsyncResult<protos::DisableTracingResponse>::Create());
}

// Called by the IPC layer.
void ConsumerIPCService::ReadBuffers(const protos::ReadBuffersRequest& req,
                                     DeferredReadBuffersResponse resp) {
  RemoteConsumer* remote_consumer = GetConsumerForCurrentRequest();
  remote_consumer->read_buffers_response = std::move(resp);
  remote_consumer->service_endpoint->ReadBuffers();
}

// Called by the IPC layer.
void ConsumerIPCService::FreeBuffers(const protos::FreeBuffersRequest& req,
                                     DeferredFreeBuffersResponse resp) {
  GetConsumerForCurrentRequest()->service_endpoint->FreeBuffers();
  resp.Resolve(ipc::AsyncResult<protos::FreeBuffersResponse>::Create());
}

////////////////////////////////////////////////////////////////////////////////
// RemoteConsumer methods
////////////////////////////////////////////////////////////////////////////////

ConsumerIPCService::RemoteConsumer::RemoteConsumer() = default;
ConsumerIPCService::RemoteConsumer::~RemoteConsumer() = default;

// Invoked by the |core_service_| business logic after the ConnectConsumer()
// call. There is nothing to do here, we really expected the ConnectConsumer()
// to just work in the local case.
void ConsumerIPCService::RemoteConsumer::OnConnect() {}

// Invoked by the |core_service_| business logic after we destroy the
// |service_endpoint| (in the RemoteConsumer dtor).
void ConsumerIPCService::RemoteConsumer::OnDisconnect() {}

void ConsumerIPCService::RemoteConsumer::OnTraceData(
    std::vector<TracePacket> trace_packets,
    bool has_more) {
  if (!read_buffers_response.IsBound())
    return;

  auto result = ipc::AsyncResult<protos::ReadBuffersResponse>::Create();
  result.set_has_more(has_more);
  // TODO(primiano): Expose the slices to the Consumer rather than stitching
  // them and wasting cpu time to hide this detail.
  for (const TracePacket& trace_packet : trace_packets) {
    std::string* dst = result->add_trace_packets();
    dst->reserve(trace_packet.size());
    for (const Slice& slice : trace_packet.slices())
      dst->append(reinterpret_cast<const char*>(slice.start), slice.size);
  }
  read_buffers_response.Resolve(std::move(result));
}

}  // namespace perfetto
