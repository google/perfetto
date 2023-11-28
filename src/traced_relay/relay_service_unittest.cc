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

#include "src/traced_relay/relay_service.h"

#include <memory>

#include "perfetto/ext/base/unix_socket.h"
#include "protos/perfetto/ipc/wire_protocol.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using ::testing::_;
using ::testing::Invoke;

class TestEventListener : public base::UnixSocket::EventListener {
 public:
  MOCK_METHOD(void, OnDataAvailable, (base::UnixSocket*), (override));
  MOCK_METHOD(void, OnConnect, (base::UnixSocket*, bool), (override));
  MOCK_METHOD(void, OnNewIncomingConnection, (base::UnixSocket*));

  void OnNewIncomingConnection(
      base::UnixSocket*,
      std::unique_ptr<base::UnixSocket> new_connection) override {
    // Need to keep |new_connection| alive.
    client_connection_ = std::move(new_connection);
    OnNewIncomingConnection(client_connection_.get());
  }

 private:
  std::unique_ptr<base::UnixSocket> client_connection_;
};

// Exercises the relay service and also validates that the relay service injects
// a SetPeerIdentity message:
//
// producer (client UnixSocket) <- @producer.sock -> relay service
// <- 127.0.0.1.* -> tcp_server (listening UnixSocet).
TEST(RelayServiceTest, SetPeerIdentity) {
  base::TestTaskRunner task_runner;
  auto relay_service = std::make_unique<RelayService>(&task_runner);

  // Set up a server UnixSocket to find an unused TCP port.
  // The TCP connection emulates the socket to the host traced.
  TestEventListener tcp_listener;
  auto tcp_server = base::UnixSocket::Listen(
      "127.0.0.1:0", &tcp_listener, &task_runner, base::SockFamily::kInet,
      base::SockType::kStream);
  ASSERT_TRUE(tcp_server->is_listening());
  auto tcp_sock_name = tcp_server->GetSockAddr();
  auto* unix_sock_name =
      "@producer.sock";  // Use abstract unix socket for server socket.

  // Start the relay service.
  relay_service->Start(unix_sock_name, tcp_sock_name.c_str());

  // Emulates the producer connection.
  TestEventListener producer_listener;
  auto producer = base::UnixSocket::Connect(
      unix_sock_name, &producer_listener, &task_runner, base::SockFamily::kUnix,
      base::SockType::kStream);
  auto producer_connected = task_runner.CreateCheckpoint("producer_connected");
  EXPECT_CALL(producer_listener, OnConnect(_, _))
      .WillOnce(Invoke([&](base::UnixSocket* s, bool conn) {
        EXPECT_TRUE(conn);
        EXPECT_EQ(s, producer.get());
        producer_connected();
      }));
  task_runner.RunUntilCheckpoint("producer_connected");

  // Add some producer data.
  ipc::Frame test_frame;
  test_frame.add_data_for_testing("test_data");
  auto test_data = ipc::BufferedFrameDeserializer::Serialize(test_frame);
  producer->SendStr(test_data);

  base::UnixSocket* tcp_client_connection = nullptr;
  auto tcp_client_connected =
      task_runner.CreateCheckpoint("tcp_client_connected");
  EXPECT_CALL(tcp_listener, OnNewIncomingConnection(_))
      .WillOnce(Invoke([&](base::UnixSocket* client) {
        tcp_client_connection = client;
        tcp_client_connected();
      }));
  task_runner.RunUntilCheckpoint("tcp_client_connected");

  // Asserts that we can receive the SetPeerIdentity message.
  auto peer_identity_recv = task_runner.CreateCheckpoint("peer_identity_recv");
  ipc::BufferedFrameDeserializer deserializer;
  EXPECT_CALL(tcp_listener, OnDataAvailable(_))
      .WillRepeatedly(Invoke([&](base::UnixSocket* tcp_conn) {
        auto buf = deserializer.BeginReceive();
        auto rsize = tcp_conn->Receive(buf.data, buf.size);
        EXPECT_TRUE(deserializer.EndReceive(rsize));

        auto frame = deserializer.PopNextFrame();
        EXPECT_TRUE(frame->has_set_peer_identity());

        const auto& set_peer_identity = frame->set_peer_identity();
        EXPECT_EQ(set_peer_identity.pid(), getpid());
        EXPECT_EQ(set_peer_identity.uid(), static_cast<int32_t>(geteuid()));

        frame = deserializer.PopNextFrame();
        EXPECT_EQ(1u, frame->data_for_testing().size());
        EXPECT_EQ(std::string("test_data"), frame->data_for_testing()[0]);

        peer_identity_recv();
      }));
  task_runner.RunUntilCheckpoint("peer_identity_recv");
}

}  // namespace
}  // namespace perfetto
