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

#include "perfetto/ipc/service_proxy.h"

#include <utility>

#include "google/protobuf/message_lite.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/ipc/service_descriptor.h"
#include "src/ipc/client_impl.h"

namespace perfetto {
namespace ipc {

ServiceProxy::ServiceProxy(EventListener* event_listener)
    : weak_ptr_factory_(this), event_listener_(event_listener) {}

ServiceProxy::~ServiceProxy() {
  if (client_ && connected())
    client_->UnbindService(service_id_);
};

void ServiceProxy::InitializeBinding(
    base::WeakPtr<Client> client,
    ServiceID service_id,
    std::map<std::string, MethodID> remote_method_ids) {
  client_ = client;
  service_id_ = service_id;
  remote_method_ids_ = std::move(remote_method_ids);
}

void ServiceProxy::BeginInvoke(const std::string& method_name,
                               const ProtoMessage& request,
                               DeferredBase reply) {
  // |reply| will auto-resolve if it gets out of scope early.
  if (!connected()) {
    PERFETTO_DCHECK(false);
    return;
  }
  if (!client_)
    return;  // The Client object has been destroyed in the meantime.

  auto remote_method_it = remote_method_ids_.find(method_name);
  RequestID request_id = 0;
  if (remote_method_it != remote_method_ids_.end()) {
    request_id =
        static_cast<ClientImpl*>(client_.get())
            ->BeginInvoke(service_id_, method_name, remote_method_it->second,
                          request, weak_ptr_factory_.GetWeakPtr());
  } else {
    PERFETTO_DLOG("Cannot find method \"%s\" on the host", method_name.c_str());
  }
  if (!request_id)
    return;
  PERFETTO_DCHECK(pending_callbacks_.count(request_id) == 0);
  pending_callbacks_.emplace(request_id, std::move(reply));
}

void ServiceProxy::EndInvoke(RequestID request_id,
                             std::unique_ptr<ProtoMessage> result,
                             bool has_more) {
  auto callback_it = pending_callbacks_.find(request_id);
  if (callback_it == pending_callbacks_.end()) {
    PERFETTO_DCHECK(false);
    return;
  }
  DeferredBase& reply_callback = callback_it->second;
  AsyncResult<ProtoMessage> reply(std::move(result), has_more);
  reply_callback.Resolve(std::move(reply));
  if (!has_more)
    pending_callbacks_.erase(callback_it);
}

void ServiceProxy::OnConnect(bool success) {
  if (success) {
    PERFETTO_DCHECK(service_id_);
    return event_listener_->OnConnect();
  }
  return event_listener_->OnDisconnect();
}

void ServiceProxy::OnDisconnect() {
  pending_callbacks_.clear();  // Will Reject() all the pending callbacks.
  event_listener_->OnDisconnect();
}

base::WeakPtr<ServiceProxy> ServiceProxy::GetWeakPtr() const {
  return weak_ptr_factory_.GetWeakPtr();
}

}  // namespace ipc
}  // namespace perfetto
