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
#include "perfetto/tracing/core/observable_events.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_stats.h"
#include "perfetto/tracing/core/tracing_service_state.h"

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
        if (weak_this)
          weak_this->OnEnableTracingResponse(std::move(response));
      });

  // |fd| will be closed when this function returns, but it's fine because the
  // IPC layer dup()'s it when sending the IPC.
  consumer_port_.EnableTracing(req, std::move(async_response), *fd);
}

void ConsumerIPCClientImpl::ChangeTraceConfig(const TraceConfig&) {
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot ChangeTraceConfig(), not connected to tracing service");
    return;
  }

  ipc::Deferred<protos::ChangeTraceConfigResponse> async_response;
  async_response.Bind(
      [](ipc::AsyncResult<protos::ChangeTraceConfigResponse> response) {
        if (!response)
          PERFETTO_DLOG("ChangeTraceConfig() failed");
      });
  protos::ChangeTraceConfigRequest req;
  consumer_port_.ChangeTraceConfig(req, std::move(async_response));
}

void ConsumerIPCClientImpl::StartTracing() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot StartTracing(), not connected to tracing service");
    return;
  }

  ipc::Deferred<protos::StartTracingResponse> async_response;
  async_response.Bind(
      [](ipc::AsyncResult<protos::StartTracingResponse> response) {
        if (!response)
          PERFETTO_DLOG("StartTracing() failed");
      });
  protos::StartTracingRequest req;
  consumer_port_.StartTracing(req, std::move(async_response));
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

void ConsumerIPCClientImpl::OnEnableTracingResponse(
    ipc::AsyncResult<protos::EnableTracingResponse> response) {
  if (!response || response->disabled())
    consumer_->OnTracingDisabled();
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

void ConsumerIPCClientImpl::Detach(const std::string& key) {
  if (!connected_) {
    PERFETTO_DLOG("Cannot Detach(), not connected to tracing service");
    return;
  }

  protos::DetachRequest req;
  req.set_key(key);
  ipc::Deferred<protos::DetachResponse> async_response;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();

  async_response.Bind(
      [weak_this](ipc::AsyncResult<protos::DetachResponse> response) {
        if (weak_this)
          weak_this->consumer_->OnDetach(!!response);
      });
  consumer_port_.Detach(req, std::move(async_response));
}

void ConsumerIPCClientImpl::Attach(const std::string& key) {
  if (!connected_) {
    PERFETTO_DLOG("Cannot Attach(), not connected to tracing service");
    return;
  }

  {
    protos::AttachRequest req;
    req.set_key(key);
    ipc::Deferred<protos::AttachResponse> async_response;
    auto weak_this = weak_ptr_factory_.GetWeakPtr();

    async_response.Bind([weak_this](
                            ipc::AsyncResult<protos::AttachResponse> response) {
      if (!weak_this)
        return;
      TraceConfig trace_config;
      if (!response) {
        weak_this->consumer_->OnAttach(/*success=*/false, trace_config);
        return;
      }
      trace_config.FromProto(response->trace_config());

      // If attached succesfully, also attach to the end-of-trace
      // notificaton callback, via EnableTracing(attach_notification_only).
      protos::EnableTracingRequest enable_req;
      enable_req.set_attach_notification_only(true);
      ipc::Deferred<protos::EnableTracingResponse> enable_resp;
      enable_resp.Bind(
          [weak_this](ipc::AsyncResult<protos::EnableTracingResponse> resp) {
            if (weak_this)
              weak_this->OnEnableTracingResponse(std::move(resp));
          });
      weak_this->consumer_port_.EnableTracing(enable_req,
                                              std::move(enable_resp));

      weak_this->consumer_->OnAttach(/*success=*/true, trace_config);
    });
    consumer_port_.Attach(req, std::move(async_response));
  }
}

void ConsumerIPCClientImpl::GetTraceStats() {
  if (!connected_) {
    PERFETTO_DLOG("Cannot GetTraceStats(), not connected to tracing service");
    return;
  }

  protos::GetTraceStatsRequest req;
  ipc::Deferred<protos::GetTraceStatsResponse> async_response;

  // The IPC layer guarantees that callbacks are destroyed after this object
  // is destroyed (by virtue of destroying the |consumer_port_|). In turn the
  // contract of this class expects the caller to not destroy the Consumer class
  // before having destroyed this class. Hence binding |this| here is safe.
  async_response.Bind(
      [this](ipc::AsyncResult<protos::GetTraceStatsResponse> response) {
        TraceStats trace_stats;
        if (!response) {
          consumer_->OnTraceStats(/*success=*/false, trace_stats);
          return;
        }
        trace_stats.FromProto(response->trace_stats());
        consumer_->OnTraceStats(/*success=*/true, trace_stats);
      });
  consumer_port_.GetTraceStats(req, std::move(async_response));
}

void ConsumerIPCClientImpl::ObserveEvents(uint32_t enabled_event_types) {
  if (!connected_) {
    PERFETTO_DLOG("Cannot ObserveEvents(), not connected to tracing service");
    return;
  }

  protos::ObserveEventsRequest req;
  if (enabled_event_types & ObservableEventType::kDataSourceInstances) {
    req.add_events_to_observe(
        protos::ObservableEvents::TYPE_DATA_SOURCES_INSTANCES);
  }
  ipc::Deferred<protos::ObserveEventsResponse> async_response;
  // The IPC layer guarantees that callbacks are destroyed after this object
  // is destroyed (by virtue of destroying the |consumer_port_|). In turn the
  // contract of this class expects the caller to not destroy the Consumer class
  // before having destroyed this class. Hence binding |this| here is safe.
  async_response.Bind(
      [this](ipc::AsyncResult<protos::ObserveEventsResponse> response) {
        // Skip empty response, which the service sends to close the stream.
        if (!response->events().instance_state_changes().size()) {
          PERFETTO_DCHECK(!response.has_more());
          return;
        }
        ObservableEvents events;
        events.FromProto(response->events());
        consumer_->OnObservableEvents(events);
      });
  consumer_port_.ObserveEvents(req, std::move(async_response));
}

void ConsumerIPCClientImpl::QueryServiceState(
    QueryServiceStateCallback callback) {
  if (!connected_) {
    PERFETTO_DLOG(
        "Cannot QueryServiceState(), not connected to tracing service");
    return;
  }

  protos::QueryServiceStateRequest req;
  ipc::Deferred<protos::QueryServiceStateResponse> async_response;
  async_response.Bind(
      [callback](ipc::AsyncResult<protos::QueryServiceStateResponse> response) {
        if (!response)
          callback(false, TracingServiceState());
        TracingServiceState svc_state;
        svc_state.FromProto(response->service_state());
        callback(true, svc_state);
      });
  consumer_port_.QueryServiceState(req, std::move(async_response));
}

}  // namespace perfetto
