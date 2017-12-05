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

#ifndef SRC_IPC_CLIENT_IMPL_H_
#define SRC_IPC_CLIENT_IMPL_H_

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ipc/client.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "src/ipc/unix_socket.h"

#include "src/ipc/wire_protocol.pb.h"

#include <list>
#include <map>
#include <memory>

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

namespace ipc {

class ServiceDescriptor;

class ClientImpl : public Client, public UnixSocket::EventListener {
 public:
  ClientImpl(const char* socket_name, base::TaskRunner*);
  ~ClientImpl() override;

  // Client implementation.
  void BindService(base::WeakPtr<ServiceProxy>) override;
  void UnbindService(ServiceID) override;
  base::ScopedFile TakeReceivedFD() override;

  // UnixSocket::EventListener implementation.
  void OnConnect(UnixSocket*, bool connected) override;
  void OnDisconnect(UnixSocket*) override;
  void OnDataAvailable(UnixSocket*) override;

  RequestID BeginInvoke(ServiceID,
                        const std::string& method_name,
                        MethodID remote_method_id,
                        const ProtoMessage& method_args,
                        base::WeakPtr<ServiceProxy>);

 private:
  struct QueuedRequest {
    QueuedRequest();
    int type = 0;  // From Frame::msg_case(), see wire_protocol.proto.
    RequestID request_id = 0;
    base::WeakPtr<ServiceProxy> service_proxy;

    // Only for type == kMsgInvokeMethod.
    std::string method_name;
  };

  ClientImpl(const ClientImpl&) = delete;
  ClientImpl& operator=(const ClientImpl&) = delete;

  bool SendFrame(const Frame&);
  void OnFrameReceived(const Frame&);
  void OnBindServiceReply(QueuedRequest, const Frame::BindServiceReply&);
  void OnInvokeMethodReply(QueuedRequest, const Frame::InvokeMethodReply&);

  std::unique_ptr<UnixSocket> sock_;
  base::TaskRunner* const task_runner_;
  RequestID last_request_id_ = 0;
  BufferedFrameDeserializer frame_deserializer_;
  base::ScopedFile received_fd_;
  std::map<RequestID, QueuedRequest> queued_requests_;
  std::map<ServiceID, base::WeakPtr<ServiceProxy>> service_bindings_;
  base::WeakPtrFactory<Client> weak_ptr_factory_;

  // Queue of calls to BindService() that happened before the socket connected.
  std::list<base::WeakPtr<ServiceProxy>> queued_bindings_;
};

}  // namespace ipc
}  // namespace perfetto

#endif  // SRC_IPC_CLIENT_IMPL_H_
