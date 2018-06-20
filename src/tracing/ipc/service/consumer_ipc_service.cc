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
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ipc/basic_types.h"
#include "perfetto/ipc/host.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/slice.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/tracing_service.h"

namespace perfetto {

ConsumerIPCService::ConsumerIPCService(TracingService* core_service)
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
  RemoteConsumer* remote_consumer = GetConsumerForCurrentRequest();
  base::ScopedFile fd;
  if (trace_config.write_into_file())
    fd = ipc::Service::TakeReceivedFD();
  remote_consumer->service_endpoint->EnableTracing(trace_config, std::move(fd));
  remote_consumer->enable_tracing_response = std::move(resp);
}

// Called by the IPC layer.
void ConsumerIPCService::DisableTracing(const protos::DisableTracingRequest&,
                                        DeferredDisableTracingResponse resp) {
  GetConsumerForCurrentRequest()->service_endpoint->DisableTracing();
  resp.Resolve(ipc::AsyncResult<protos::DisableTracingResponse>::Create());
}

// Called by the IPC layer.
void ConsumerIPCService::ReadBuffers(const protos::ReadBuffersRequest&,
                                     DeferredReadBuffersResponse resp) {
  RemoteConsumer* remote_consumer = GetConsumerForCurrentRequest();
  remote_consumer->read_buffers_response = std::move(resp);
  remote_consumer->service_endpoint->ReadBuffers();
}

// Called by the IPC layer.
void ConsumerIPCService::FreeBuffers(const protos::FreeBuffersRequest&,
                                     DeferredFreeBuffersResponse resp) {
  GetConsumerForCurrentRequest()->service_endpoint->FreeBuffers();
  resp.Resolve(ipc::AsyncResult<protos::FreeBuffersResponse>::Create());
}

// Called by the IPC layer.
void ConsumerIPCService::Flush(const protos::FlushRequest& req,
                               DeferredFlushResponse resp) {
  auto it = pending_flush_responses_.insert(pending_flush_responses_.end(),
                                            std::move(resp));
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  auto callback = [weak_this, it](bool success) {
    if (weak_this)
      weak_this->OnFlushCallback(success, std::move(it));
  };
  GetConsumerForCurrentRequest()->service_endpoint->Flush(req.timeout_ms(),
                                                          std::move(callback));
}

// Called by the service in response to a service_endpoint->Flush() request.
void ConsumerIPCService::OnFlushCallback(
    bool success,
    PendingFlushResponses::iterator pending_response_it) {
  DeferredFlushResponse response(std::move(*pending_response_it));
  pending_flush_responses_.erase(pending_response_it);
  if (success) {
    response.Resolve(ipc::AsyncResult<protos::FlushResponse>::Create());
  } else {
    response.Reject();
  }
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

void ConsumerIPCService::RemoteConsumer::OnTracingDisabled() {
  auto result = ipc::AsyncResult<protos::EnableTracingResponse>::Create();
  result->set_disabled(true);
  enable_tracing_response.Resolve(std::move(result));
}

void ConsumerIPCService::RemoteConsumer::OnTraceData(
    std::vector<TracePacket> trace_packets,
    bool has_more) {
  if (!read_buffers_response.IsBound())
    return;

  auto result = ipc::AsyncResult<protos::ReadBuffersResponse>::Create();

  // A TracePacket might be too big to fit into a single IPC message (max
  // kIPCBufferSize). However a TracePacket is made of slices and each slice
  // is way smaller than kIPCBufferSize (a slice size is effectively bounded by
  // the max chunk size of the SharedMemoryABI). When sending a TracePacket,
  // if its slices don't fit within one IPC, chunk them over several contiguous
  // IPCs using the |last_slice_for_packet| for glueing on the other side.
  static_assert(ipc::kIPCBufferSize >= SharedMemoryABI::kMaxPageSize * 2,
                "kIPCBufferSize too small given the max possible slice size");

  auto send_ipc_reply = [this, &result](bool more) {
    result.set_has_more(more);
    read_buffers_response.Resolve(std::move(result));
    result = ipc::AsyncResult<protos::ReadBuffersResponse>::Create();
  };

  size_t approx_reply_size = 0;
  for (const TracePacket& trace_packet : trace_packets) {
    size_t num_slices_left_for_packet = trace_packet.slices().size();
    for (const Slice& slice : trace_packet.slices()) {
      // Check if this slice would cause the IPC to overflow its max size and,
      // if that is the case, split the IPCs. The "16" and "64" below are
      // over-estimations of, respectively:
      // 16: the preamble that prefixes each slice (there are 2 x size fields
      //     in the proto + the |last_slice_for_packet| bool).
      // 64: the overhead of the IPC InvokeMethodReply + wire_protocol's frame.
      // If these estimations are wrong, BufferedFrameDeserializer::Serialize()
      // will hit a DCHECK anyways.
      const size_t approx_slice_size = slice.size + 16;
      if (approx_reply_size + approx_slice_size > ipc::kIPCBufferSize - 64) {
        // If we hit this CHECK we got a single slice that is > kIPCBufferSize.
        PERFETTO_CHECK(result->slices_size() > 0);
        send_ipc_reply(/*has_more=*/true);
        approx_reply_size = 0;
      }
      approx_reply_size += approx_slice_size;

      auto* res_slice = result->add_slices();
      res_slice->set_last_slice_for_packet(--num_slices_left_for_packet == 0);
      res_slice->set_data(slice.start, slice.size);
    }
  }
  send_ipc_reply(has_more);
}

}  // namespace perfetto
