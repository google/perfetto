/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACED_RELAY_RELAY_SERVICE_H_
#define SRC_TRACED_RELAY_RELAY_SERVICE_H_

#include <memory>
#include <vector>

#include "perfetto/ext/base/unix_socket.h"
#include "src/traced_relay/socket_relay_handler.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base.

// A class for relaying the producer data between the local producers and the
// remote tracing service.
class RelayService : public base::UnixSocket::EventListener {
 public:
  explicit RelayService(base::TaskRunner* task_runner);
  ~RelayService() override = default;

  // Starts the service relay that forwards messages between the
  // |server_socket_name| and |client_socket_name| ports.
  void Start(const char* server_socket_name, const char* client_socket_name);

 private:
  struct PendingConnection {
    // This keeps a connected UnixSocketRaw server socket in its first element.
    std::unique_ptr<SocketPair> socket_pair;
    // This keeps the connecting client connection.
    std::unique_ptr<base::UnixSocket> connecting_client_conn;
  };

  RelayService(const RelayService&) = delete;
  RelayService& operator=(const RelayService&) = delete;

  // UnixSocket::EventListener implementation.
  void OnNewIncomingConnection(base::UnixSocket*,
                               std::unique_ptr<base::UnixSocket>) override;
  void OnConnect(base::UnixSocket* self, bool connected) override;
  void OnDisconnect(base::UnixSocket* self) override;
  void OnDataAvailable(base::UnixSocket* self) override;

  base::TaskRunner* const task_runner_ = nullptr;

  std::unique_ptr<base::UnixSocket> listening_socket_;
  std::string client_socket_name_;

  // Keeps the socket pairs while waiting for relay connections to be
  // established.
  std::vector<PendingConnection> pending_connections_;

  SocketRelayHandler socket_relay_handler_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_RELAY_RELAY_SERVICE_H_
