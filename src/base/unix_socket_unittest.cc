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

#include "perfetto/ext/base/unix_socket.h"

#include <signal.h>
#include <sys/types.h>
#include <list>
#include <thread>

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/periodic_task.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Mock;

ipc::TestSocket kTestSocket{"unix_socket_unittest"};

class MockEventListener : public UnixSocket::EventListener {
 public:
  MOCK_METHOD2(OnNewIncomingConnection, void(UnixSocket*, UnixSocket*));
  MOCK_METHOD2(OnConnect, void(UnixSocket*, bool));
  MOCK_METHOD1(OnDisconnect, void(UnixSocket*));
  MOCK_METHOD1(OnDataAvailable, void(UnixSocket*));

  // GMock doesn't support mocking methods with non-copiable args.
  void OnNewIncomingConnection(
      UnixSocket* self,
      std::unique_ptr<UnixSocket> new_connection) override {
    incoming_connections_.emplace_back(std::move(new_connection));
    OnNewIncomingConnection(self, incoming_connections_.back().get());
  }

  std::unique_ptr<UnixSocket> GetIncomingConnection() {
    if (incoming_connections_.empty())
      return nullptr;
    std::unique_ptr<UnixSocket> sock = std::move(incoming_connections_.front());
    incoming_connections_.pop_front();
    return sock;
  }

 private:
  std::list<std::unique_ptr<UnixSocket>> incoming_connections_;
};

class UnixSocketTest : public ::testing::Test {
 protected:
  void SetUp() override { kTestSocket.Destroy(); }
  void TearDown() override { kTestSocket.Destroy(); }

  TestTaskRunner task_runner_;
  MockEventListener event_listener_;
};

TEST_F(UnixSocketTest, ConnectionFailureIfUnreachable) {
  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  ASSERT_FALSE(cli->is_connected());
  auto checkpoint = task_runner_.CreateCheckpoint("failure");
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), false))
      .WillOnce(InvokeWithoutArgs(checkpoint));
  task_runner_.RunUntilCheckpoint("failure");
}

// Both server and client should see an OnDisconnect() if the server drops
// incoming connections immediately as they are created.
TEST_F(UnixSocketTest, ConnectionImmediatelyDroppedByServer) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());

  // The server will immediately shutdown the connection upon
  // OnNewIncomingConnection().
  auto srv_did_shutdown = task_runner_.CreateCheckpoint("srv_did_shutdown");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(
          Invoke([this, srv_did_shutdown](UnixSocket*, UnixSocket* new_conn) {
            EXPECT_CALL(event_listener_, OnDisconnect(new_conn));
            new_conn->Shutdown(true);
            srv_did_shutdown();
          }));

  auto checkpoint = task_runner_.CreateCheckpoint("cli_connected");
  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(checkpoint));
  task_runner_.RunUntilCheckpoint("cli_connected");
  task_runner_.RunUntilCheckpoint("srv_did_shutdown");

  // Trying to send something will trigger the disconnection notification.
  auto cli_disconnected = task_runner_.CreateCheckpoint("cli_disconnected");
  EXPECT_CALL(event_listener_, OnDisconnect(cli.get()))
      .WillOnce(InvokeWithoutArgs(cli_disconnected));

  // On Windows the first send immediately after the disconnection succeeds, the
  // kernel will detect the disconnection only later.
  cli->SendStr(".");
  EXPECT_FALSE(cli->SendStr("should_fail_both_on_win_and_unix"));
  task_runner_.RunUntilCheckpoint("cli_disconnected");
}

TEST_F(UnixSocketTest, ClientAndServerExchangeData) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());

  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(cli_connected));
  auto srv_conn_seen = task_runner_.CreateCheckpoint("srv_conn_seen");
  auto srv_disconnected = task_runner_.CreateCheckpoint("srv_disconnected");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, srv_conn_seen, srv_disconnected](
                           UnixSocket*, UnixSocket* srv_conn) {
        EXPECT_CALL(event_listener_, OnDisconnect(srv_conn))
            .WillOnce(InvokeWithoutArgs(srv_disconnected));
        srv_conn_seen();
      }));
  task_runner_.RunUntilCheckpoint("srv_conn_seen");
  task_runner_.RunUntilCheckpoint("cli_connected");

  auto srv_conn = event_listener_.GetIncomingConnection();
  ASSERT_TRUE(srv_conn);
  ASSERT_TRUE(cli->is_connected());

  auto cli_did_recv = task_runner_.CreateCheckpoint("cli_did_recv");
  EXPECT_CALL(event_listener_, OnDataAvailable(cli.get()))
      .WillOnce(Invoke([cli_did_recv](UnixSocket* s) {
        ASSERT_EQ("srv>cli", s->ReceiveString());
        cli_did_recv();
      }));

  auto srv_did_recv = task_runner_.CreateCheckpoint("srv_did_recv");
  EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn.get()))
      .WillOnce(Invoke([srv_did_recv](UnixSocket* s) {
        ASSERT_EQ("cli>srv", s->ReceiveString());
        srv_did_recv();
      }));
  ASSERT_TRUE(cli->SendStr("cli>srv"));
  ASSERT_TRUE(srv_conn->SendStr("srv>cli"));
  task_runner_.RunUntilCheckpoint("cli_did_recv");
  task_runner_.RunUntilCheckpoint("srv_did_recv");

  // Check that Send/Receive() fails gracefully once the socket is closed.
  auto cli_disconnected = task_runner_.CreateCheckpoint("cli_disconnected");
  EXPECT_CALL(event_listener_, OnDisconnect(cli.get()))
      .WillOnce(InvokeWithoutArgs(cli_disconnected));
  cli->Shutdown(true);
  char msg[4];
  ASSERT_EQ(0u, cli->Receive(&msg, sizeof(msg)));
  ASSERT_EQ("", cli->ReceiveString());
  ASSERT_EQ(0u, srv_conn->Receive(&msg, sizeof(msg)));
  ASSERT_EQ("", srv_conn->ReceiveString());
  ASSERT_FALSE(cli->SendStr("foo"));
  ASSERT_FALSE(srv_conn->SendStr("bar"));
  srv->Shutdown(true);
  task_runner_.RunUntilCheckpoint("cli_disconnected");
  task_runner_.RunUntilCheckpoint("srv_disconnected");
}

TEST_F(UnixSocketTest, ListenWithPassedSocketHandle) {
  auto sock_raw =
      UnixSocketRaw::CreateMayFail(kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(sock_raw.Bind(kTestSocket.name()));
  auto fd = sock_raw.ReleaseFd();
  auto srv = UnixSocket::Listen(std::move(fd), &event_listener_, &task_runner_,
                                kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());

  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(cli_connected));
  auto srv_connected = task_runner_.CreateCheckpoint("srv_connected");
  auto srv_disconnected = task_runner_.CreateCheckpoint("srv_disconnected");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, srv_connected, srv_disconnected](
                           UnixSocket*, UnixSocket* srv_conn) {
        // An empty OnDataAvailable might be raised to signal the EOF state.
        EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn))
            .WillRepeatedly(
                InvokeWithoutArgs([srv_conn] { srv_conn->ReceiveString(); }));
        EXPECT_CALL(event_listener_, OnDisconnect(srv_conn))
            .WillOnce(InvokeWithoutArgs(srv_disconnected));
        srv_connected();
      }));
  task_runner_.RunUntilCheckpoint("srv_connected");
  task_runner_.RunUntilCheckpoint("cli_connected");
  ASSERT_TRUE(cli->is_connected());
  cli.reset();
  task_runner_.RunUntilCheckpoint("srv_disconnected");
}

// Mostly a stress tests. Connects kNumClients clients to the same server and
// tests that all can exchange data and can see the expected sequence of events.
TEST_F(UnixSocketTest, SeveralClients) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());
  constexpr size_t kNumClients = 32;
  std::unique_ptr<UnixSocket> cli[kNumClients];

  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .Times(kNumClients)
      .WillRepeatedly(Invoke([this](UnixSocket*, UnixSocket* s) {
        EXPECT_CALL(event_listener_, OnDataAvailable(s))
            .WillOnce(Invoke([](UnixSocket* t) {
              ASSERT_EQ("PING", t->ReceiveString());
              ASSERT_TRUE(t->SendStr("PONG"));
            }));
      }));

  for (size_t i = 0; i < kNumClients; i++) {
    cli[i] =
        UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                            kTestSocket.family(), SockType::kStream);
    EXPECT_CALL(event_listener_, OnConnect(cli[i].get(), true))
        .WillOnce(Invoke([](UnixSocket* s, bool success) {
          ASSERT_TRUE(success);
          ASSERT_TRUE(s->SendStr("PING"));
        }));

    auto checkpoint = task_runner_.CreateCheckpoint(std::to_string(i));
    EXPECT_CALL(event_listener_, OnDataAvailable(cli[i].get()))
        .WillOnce(Invoke([checkpoint](UnixSocket* s) {
          ASSERT_EQ("PONG", s->ReceiveString());
          checkpoint();
        }));
  }

  for (size_t i = 0; i < kNumClients; i++) {
    task_runner_.RunUntilCheckpoint(std::to_string(i));
    ASSERT_TRUE(Mock::VerifyAndClearExpectations(cli[i].get()));
  }
}

TEST_F(UnixSocketTest, BlockingSend) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());

  auto all_frames_done = task_runner_.CreateCheckpoint("all_frames_done");
  size_t total_bytes_received = 0;
  static constexpr size_t kTotalBytes = 1024 * 1024 * 4;
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, &total_bytes_received, all_frames_done](
                           UnixSocket*, UnixSocket* srv_conn) {
        EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn))
            .WillRepeatedly(
                Invoke([&total_bytes_received, all_frames_done](UnixSocket* s) {
                  char buf[1024];
                  size_t res = s->Receive(buf, sizeof(buf));
                  total_bytes_received += res;
                  if (total_bytes_received == kTotalBytes)
                    all_frames_done();
                }));
      }));

  // Override default timeout as this test can take time on the emulator.
  static constexpr int kTimeoutMs = 60000 * 3;

  // Perform the blocking send form another thread.
  std::thread tx_thread([] {
    TestTaskRunner tx_task_runner;
    MockEventListener tx_events;
    auto cli =
        UnixSocket::Connect(kTestSocket.name(), &tx_events, &tx_task_runner,
                            kTestSocket.family(), SockType::kStream);

    auto cli_connected = tx_task_runner.CreateCheckpoint("cli_connected");
    EXPECT_CALL(tx_events, OnConnect(cli.get(), true))
        .WillOnce(InvokeWithoutArgs(cli_connected));
    tx_task_runner.RunUntilCheckpoint("cli_connected");

    auto all_sent = tx_task_runner.CreateCheckpoint("all_sent");
    std::string buf(1024 * 32, '\0');
    tx_task_runner.PostTask([&cli, &buf, all_sent] {
      for (size_t i = 0; i < kTotalBytes / buf.size(); i++)
        cli->Send(buf.data(), buf.size());
      all_sent();
    });
    tx_task_runner.RunUntilCheckpoint("all_sent", kTimeoutMs);
  });

  task_runner_.RunUntilCheckpoint("all_frames_done", kTimeoutMs);
  tx_thread.join();
}

// Regression test for b/76155349 . If the receiver end disconnects while the
// sender is in the middle of a large send(), the socket should gracefully give
// up (i.e. Shutdown()) but not crash.
TEST_F(UnixSocketTest, ReceiverDisconnectsDuringSend) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());
  static constexpr int kTimeoutMs = 30000;

  auto receive_done = task_runner_.CreateCheckpoint("receive_done");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, receive_done](UnixSocket*, UnixSocket* srv_conn) {
        EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn))
            .WillOnce(Invoke([receive_done](UnixSocket* s) {
              char buf[1024];
              size_t res = s->Receive(buf, sizeof(buf));
              ASSERT_EQ(1024u, res);
              s->Shutdown(false /*notify*/);
              receive_done();
            }));
      }));

  // Perform the blocking send form another thread.
  std::thread tx_thread([] {
    TestTaskRunner tx_task_runner;
    MockEventListener tx_events;
    auto cli =
        UnixSocket::Connect(kTestSocket.name(), &tx_events, &tx_task_runner,
                            kTestSocket.family(), SockType::kStream);

    auto cli_connected = tx_task_runner.CreateCheckpoint("cli_connected");
    EXPECT_CALL(tx_events, OnConnect(cli.get(), true))
        .WillOnce(InvokeWithoutArgs(cli_connected));
    tx_task_runner.RunUntilCheckpoint("cli_connected");

    auto send_done = tx_task_runner.CreateCheckpoint("send_done");
    static constexpr size_t kBufSize = 32 * 1024 * 1024;
    std::unique_ptr<char[]> buf(new char[kBufSize]());
    tx_task_runner.PostTask([&cli, &buf, send_done] {
      cli->Send(buf.get(), kBufSize);
      send_done();
    });

    tx_task_runner.RunUntilCheckpoint("send_done", kTimeoutMs);
  });
  task_runner_.RunUntilCheckpoint("receive_done", kTimeoutMs);
  tx_thread.join();
}

TEST_F(UnixSocketTest, ReleaseSocket) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());
  auto srv_connected = task_runner_.CreateCheckpoint("srv_connected");
  UnixSocket* peer = nullptr;
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(
          Invoke([srv_connected, &peer](UnixSocket*, UnixSocket* new_conn) {
            peer = new_conn;
            srv_connected();
          }));

  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(cli_connected));
  task_runner_.RunUntilCheckpoint("srv_connected");
  task_runner_.RunUntilCheckpoint("cli_connected");
  srv->Shutdown(true);

  cli->SendStr("test");

  ASSERT_NE(peer, nullptr);
  auto raw_sock = peer->ReleaseSocket();

  EXPECT_CALL(event_listener_, OnDataAvailable(_)).Times(0);
  task_runner_.RunUntilIdle();

  char buf[5];
  ASSERT_TRUE(raw_sock);
  ASSERT_EQ(raw_sock.Receive(buf, sizeof(buf)), 4);
  buf[sizeof(buf) - 1] = '\0';
  ASSERT_STREQ(buf, "test");
}

TEST_F(UnixSocketTest, TcpStream) {
  char host_and_port[32];
  int attempt = 0;
  std::unique_ptr<UnixSocket> srv;

  // Try listening on a random port. Some ports might be taken by other syste
  // services. Do a bunch of attempts on different ports before giving up.
  do {
    base::SprintfTrunc(host_and_port, sizeof(host_and_port), "127.0.0.1:%d",
                       10000 + (rand() % 10000));
    srv = UnixSocket::Listen(host_and_port, &event_listener_, &task_runner_,
                             SockFamily::kInet, SockType::kStream);
  } while ((!srv || !srv->is_listening()) && attempt++ < 10);
  ASSERT_TRUE(srv->is_listening());

  constexpr size_t kNumClients = 3;
  std::unique_ptr<UnixSocket> cli[kNumClients];
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .Times(kNumClients)
      .WillRepeatedly(Invoke([&](UnixSocket*, UnixSocket* s) {
        // OnDisconnect() might spuriously happen depending on the dtor order.
        EXPECT_CALL(event_listener_, OnDisconnect(s)).Times(AtLeast(0));
        EXPECT_CALL(event_listener_, OnDataAvailable(s))
            .WillRepeatedly(Invoke([](UnixSocket* cli_sock) {
              cli_sock->ReceiveString();  // Read connection EOF;
            }));
        ASSERT_TRUE(s->SendStr("welcome"));
      }));

  for (size_t i = 0; i < kNumClients; i++) {
    cli[i] = UnixSocket::Connect(host_and_port, &event_listener_, &task_runner_,
                                 SockFamily::kInet, SockType::kStream);
    auto checkpoint = task_runner_.CreateCheckpoint(std::to_string(i));
    EXPECT_CALL(event_listener_, OnDisconnect(cli[i].get())).Times(AtLeast(0));
    EXPECT_CALL(event_listener_, OnConnect(cli[i].get(), true));
    EXPECT_CALL(event_listener_, OnDataAvailable(cli[i].get()))
        .WillRepeatedly(Invoke([checkpoint](UnixSocket* s) {
          auto str = s->ReceiveString();
          if (str == "")
            return;  // Connection EOF.
          ASSERT_EQ("welcome", str);
          checkpoint();
        }));
  }

  for (size_t i = 0; i < kNumClients; i++) {
    task_runner_.RunUntilCheckpoint(std::to_string(i));
    ASSERT_TRUE(Mock::VerifyAndClearExpectations(cli[i].get()));
  }
}

// ---------------------------------
// Posix-only tests below this point
// ---------------------------------

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

// Tests the SockPeerCredMode::kIgnore logic.
TEST_F(UnixSocketTest, IgnorePeerCredentials) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());
  auto cli1_connected = task_runner_.CreateCheckpoint("cli1_connected");
  auto cli1 = UnixSocket::Connect(kTestSocket.name(), &event_listener_,
                                  &task_runner_, kTestSocket.family(),
                                  SockType::kStream, SockPeerCredMode::kIgnore);
  EXPECT_CALL(event_listener_, OnConnect(cli1.get(), true))
      .WillOnce(InvokeWithoutArgs(cli1_connected));

  auto cli2_connected = task_runner_.CreateCheckpoint("cli2_connected");
  auto cli2 = UnixSocket::Connect(
      kTestSocket.name(), &event_listener_, &task_runner_, kTestSocket.family(),
      SockType::kStream, SockPeerCredMode::kReadOnConnect);
  EXPECT_CALL(event_listener_, OnConnect(cli2.get(), true))
      .WillOnce(InvokeWithoutArgs(cli2_connected));

  task_runner_.RunUntilCheckpoint("cli1_connected");
  task_runner_.RunUntilCheckpoint("cli2_connected");

  ASSERT_EQ(cli1->peer_uid_posix(/*skip_check_for_testing=*/true), kInvalidUid);
  ASSERT_EQ(cli2->peer_uid_posix(), geteuid());
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  ASSERT_EQ(cli1->peer_pid_linux(/*skip_check_for_testing=*/true), kInvalidPid);
  ASSERT_EQ(cli2->peer_pid_linux(), getpid());
#endif
}

// Checks that the peer_uid() is retained after the client disconnects. The IPC
// layer needs to rely on this to validate messages received immediately before
// a client disconnects.
TEST_F(UnixSocketTest, PeerCredentialsRetainedAfterDisconnect) {
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());
  UnixSocket* srv_client_conn = nullptr;
  auto srv_connected = task_runner_.CreateCheckpoint("srv_connected");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([&srv_client_conn, srv_connected](UnixSocket*,
                                                         UnixSocket* srv_conn) {
        srv_client_conn = srv_conn;
        EXPECT_EQ(geteuid(), static_cast<uint32_t>(srv_conn->peer_uid_posix()));
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
        EXPECT_EQ(getpid(), static_cast<pid_t>(srv_conn->peer_pid_linux()));
#endif
        srv_connected();
      }));
  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(cli_connected));

  task_runner_.RunUntilCheckpoint("cli_connected");
  task_runner_.RunUntilCheckpoint("srv_connected");
  ASSERT_NE(nullptr, srv_client_conn);
  ASSERT_TRUE(srv_client_conn->is_connected());

  auto cli_disconnected = task_runner_.CreateCheckpoint("cli_disconnected");
  EXPECT_CALL(event_listener_, OnDisconnect(srv_client_conn))
      .WillOnce(InvokeWithoutArgs(cli_disconnected));

  // TODO(primiano): when the a peer disconnects, the other end receives a
  // spurious OnDataAvailable() that needs to be acked with a Receive() to read
  // the EOF. See b/69536434.
  EXPECT_CALL(event_listener_, OnDataAvailable(srv_client_conn))
      .WillOnce(Invoke([](UnixSocket* sock) { sock->ReceiveString(); }));

  cli.reset();
  task_runner_.RunUntilCheckpoint("cli_disconnected");
  ASSERT_FALSE(srv_client_conn->is_connected());
  EXPECT_EQ(geteuid(),
            static_cast<uint32_t>(srv_client_conn->peer_uid_posix()));
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  EXPECT_EQ(getpid(), static_cast<pid_t>(srv_client_conn->peer_pid_linux()));
#endif
}

TEST_F(UnixSocketTest, ClientAndServerExchangeFDs) {
  static constexpr char cli_str[] = "cli>srv";
  static constexpr char srv_str[] = "srv>cli";
  auto srv =
      UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                         kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(srv->is_listening());

  auto cli =
      UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                          kTestSocket.family(), SockType::kStream);
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true));
  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  auto srv_disconnected = task_runner_.CreateCheckpoint("srv_disconnected");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, cli_connected, srv_disconnected](
                           UnixSocket*, UnixSocket* srv_conn) {
        EXPECT_CALL(event_listener_, OnDisconnect(srv_conn))
            .WillOnce(InvokeWithoutArgs(srv_disconnected));
        cli_connected();
      }));
  task_runner_.RunUntilCheckpoint("cli_connected");

  auto srv_conn = event_listener_.GetIncomingConnection();
  ASSERT_TRUE(srv_conn);
  ASSERT_TRUE(cli->is_connected());

  ScopedFile null_fd(base::OpenFile("/dev/null", O_RDONLY));
  ScopedFile zero_fd(base::OpenFile("/dev/zero", O_RDONLY));

  auto cli_did_recv = task_runner_.CreateCheckpoint("cli_did_recv");
  EXPECT_CALL(event_listener_, OnDataAvailable(cli.get()))
      .WillRepeatedly(Invoke([cli_did_recv](UnixSocket* s) {
        ScopedFile fd_buf[3];
        char buf[sizeof(cli_str)];
        if (!s->Receive(buf, sizeof(buf), fd_buf, ArraySize(fd_buf)))
          return;
        ASSERT_STREQ(srv_str, buf);
        ASSERT_NE(*fd_buf[0], -1);
        ASSERT_NE(*fd_buf[1], -1);
        ASSERT_EQ(*fd_buf[2], -1);

        char rd_buf[1];
        // /dev/null
        ASSERT_EQ(read(*fd_buf[0], rd_buf, sizeof(rd_buf)), 0);
        // /dev/zero
        ASSERT_EQ(read(*fd_buf[1], rd_buf, sizeof(rd_buf)), 1);
        cli_did_recv();
      }));

  auto srv_did_recv = task_runner_.CreateCheckpoint("srv_did_recv");
  EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn.get()))
      .WillRepeatedly(Invoke([srv_did_recv](UnixSocket* s) {
        ScopedFile fd_buf[3];
        char buf[sizeof(srv_str)];
        if (!s->Receive(buf, sizeof(buf), fd_buf, ArraySize(fd_buf)))
          return;
        ASSERT_STREQ(cli_str, buf);
        ASSERT_NE(*fd_buf[0], -1);
        ASSERT_NE(*fd_buf[1], -1);
        ASSERT_EQ(*fd_buf[2], -1);

        char rd_buf[1];
        // /dev/null
        ASSERT_EQ(read(*fd_buf[0], rd_buf, sizeof(rd_buf)), 0);
        // /dev/zero
        ASSERT_EQ(read(*fd_buf[1], rd_buf, sizeof(rd_buf)), 1);
        srv_did_recv();
      }));

  int buf_fd[2] = {null_fd.get(), zero_fd.get()};

  ASSERT_TRUE(
      cli->Send(cli_str, sizeof(cli_str), buf_fd, base::ArraySize(buf_fd)));
  ASSERT_TRUE(srv_conn->Send(srv_str, sizeof(srv_str), buf_fd,
                             base::ArraySize(buf_fd)));
  task_runner_.RunUntilCheckpoint("srv_did_recv");
  task_runner_.RunUntilCheckpoint("cli_did_recv");

  auto cli_disconnected = task_runner_.CreateCheckpoint("cli_disconnected");
  EXPECT_CALL(event_listener_, OnDisconnect(cli.get()))
      .WillOnce(InvokeWithoutArgs(cli_disconnected));
  cli->Shutdown(true);
  srv->Shutdown(true);
  task_runner_.RunUntilCheckpoint("srv_disconnected");
  task_runner_.RunUntilCheckpoint("cli_disconnected");
}

// Creates two processes. The server process creates a file and passes it over
// the socket to the client. Both processes mmap the file in shared mode and
// check that they see the same contents.
TEST_F(UnixSocketTest, SharedMemory) {
  Pipe pipe = Pipe::Create();
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  constexpr size_t kTmpSize = 4096;

  if (pid == 0) {
    // Child process.
    TempFile scoped_tmp = TempFile::CreateUnlinked();
    int tmp_fd = scoped_tmp.fd();
    ASSERT_FALSE(ftruncate(tmp_fd, kTmpSize));
    char* mem = reinterpret_cast<char*>(
        mmap(nullptr, kTmpSize, PROT_READ | PROT_WRITE, MAP_SHARED, tmp_fd, 0));
    ASSERT_NE(nullptr, mem);
    memcpy(mem, "shm rocks", 10);

    auto srv =
        UnixSocket::Listen(kTestSocket.name(), &event_listener_, &task_runner_,
                           kTestSocket.family(), SockType::kStream);
    ASSERT_TRUE(srv->is_listening());
    // Signal the other process that it can connect.
    ASSERT_EQ(1, base::WriteAll(*pipe.wr, ".", 1));
    auto checkpoint = task_runner_.CreateCheckpoint("change_seen_by_server");
    EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
        .WillOnce(Invoke(
            [this, tmp_fd, checkpoint, mem](UnixSocket*, UnixSocket* new_conn) {
              ASSERT_EQ(geteuid(),
                        static_cast<uint32_t>(new_conn->peer_uid_posix()));
              ASSERT_TRUE(new_conn->Send("txfd", 5, tmp_fd));
              // Wait for the client to change this again.
              EXPECT_CALL(event_listener_, OnDataAvailable(new_conn))
                  .WillOnce(Invoke([checkpoint, mem](UnixSocket* s) {
                    ASSERT_EQ("change notify", s->ReceiveString());
                    ASSERT_STREQ("rock more", mem);
                    checkpoint();
                  }));
            }));
    task_runner_.RunUntilCheckpoint("change_seen_by_server");
    ASSERT_TRUE(Mock::VerifyAndClearExpectations(&event_listener_));
    _exit(0);
  } else {
    char sync_cmd = '\0';
    ASSERT_EQ(1, PERFETTO_EINTR(read(*pipe.rd, &sync_cmd, 1)));
    ASSERT_EQ('.', sync_cmd);
    auto cli =
        UnixSocket::Connect(kTestSocket.name(), &event_listener_, &task_runner_,
                            kTestSocket.family(), SockType::kStream);
    EXPECT_CALL(event_listener_, OnConnect(cli.get(), true));
    auto checkpoint = task_runner_.CreateCheckpoint("change_seen_by_client");
    EXPECT_CALL(event_listener_, OnDataAvailable(cli.get()))
        .WillOnce(Invoke([checkpoint](UnixSocket* s) {
          char msg[32];
          ScopedFile fd;
          ASSERT_EQ(5u, s->Receive(msg, sizeof(msg), &fd));
          ASSERT_STREQ("txfd", msg);
          ASSERT_TRUE(fd);
          char* mem = reinterpret_cast<char*>(mmap(
              nullptr, kTmpSize, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0));
          ASSERT_NE(nullptr, mem);
          mem[9] = '\0';  // Just to get a clean error in case of test failure.
          ASSERT_STREQ("shm rocks", mem);

          // Now change the shared memory and ping the other process.
          memcpy(mem, "rock more", 10);
          ASSERT_TRUE(s->SendStr("change notify"));
          checkpoint();
        }));
    task_runner_.RunUntilCheckpoint("change_seen_by_client");
    int st = 0;
    PERFETTO_EINTR(waitpid(pid, &st, 0));
    ASSERT_FALSE(WIFSIGNALED(st)) << "Server died with signal " << WTERMSIG(st);
    EXPECT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
  }
}

TEST_F(UnixSocketTest, ShiftMsgHdrSendPartialFirst) {
  // Send a part of the first iov, then send the rest.
  struct iovec iov[2] = {};
  char hello[] = "hello";
  char world[] = "world";
  iov[0].iov_base = &hello[0];
  iov[0].iov_len = base::ArraySize(hello);

  iov[1].iov_base = &world[0];
  iov[1].iov_len = base::ArraySize(world);

  struct msghdr hdr = {};
  hdr.msg_iov = iov;
  hdr.msg_iovlen = base::ArraySize(iov);

  UnixSocketRaw::ShiftMsgHdrPosix(1, &hdr);
  EXPECT_NE(hdr.msg_iov, nullptr);
  EXPECT_EQ(hdr.msg_iov[0].iov_base, &hello[1]);
  EXPECT_EQ(hdr.msg_iov[1].iov_base, &world[0]);
  EXPECT_EQ(static_cast<int>(hdr.msg_iovlen), 2);
  EXPECT_STREQ(reinterpret_cast<char*>(hdr.msg_iov[0].iov_base), "ello");
  EXPECT_EQ(iov[0].iov_len, base::ArraySize(hello) - 1);

  UnixSocketRaw::ShiftMsgHdrPosix(base::ArraySize(hello) - 1, &hdr);
  EXPECT_EQ(hdr.msg_iov, &iov[1]);
  EXPECT_EQ(static_cast<int>(hdr.msg_iovlen), 1);
  EXPECT_STREQ(reinterpret_cast<char*>(hdr.msg_iov[0].iov_base), world);
  EXPECT_EQ(hdr.msg_iov[0].iov_len, base::ArraySize(world));

  UnixSocketRaw::ShiftMsgHdrPosix(base::ArraySize(world), &hdr);
  EXPECT_EQ(hdr.msg_iov, nullptr);
  EXPECT_EQ(static_cast<int>(hdr.msg_iovlen), 0);
}

TEST_F(UnixSocketTest, ShiftMsgHdrSendFirstAndPartial) {
  // Send first iov and part of the second iov, then send the rest.
  struct iovec iov[2] = {};
  char hello[] = "hello";
  char world[] = "world";
  iov[0].iov_base = &hello[0];
  iov[0].iov_len = base::ArraySize(hello);

  iov[1].iov_base = &world[0];
  iov[1].iov_len = base::ArraySize(world);

  struct msghdr hdr = {};
  hdr.msg_iov = iov;
  hdr.msg_iovlen = base::ArraySize(iov);

  UnixSocketRaw::ShiftMsgHdrPosix(base::ArraySize(hello) + 1, &hdr);
  EXPECT_NE(hdr.msg_iov, nullptr);
  EXPECT_EQ(static_cast<int>(hdr.msg_iovlen), 1);
  EXPECT_STREQ(reinterpret_cast<char*>(hdr.msg_iov[0].iov_base), "orld");
  EXPECT_EQ(hdr.msg_iov[0].iov_len, base::ArraySize(world) - 1);

  UnixSocketRaw::ShiftMsgHdrPosix(base::ArraySize(world) - 1, &hdr);
  EXPECT_EQ(hdr.msg_iov, nullptr);
  EXPECT_EQ(static_cast<int>(hdr.msg_iovlen), 0);
}

TEST_F(UnixSocketTest, ShiftMsgHdrSendEverything) {
  // Send everything at once.
  struct iovec iov[2] = {};
  char hello[] = "hello";
  char world[] = "world";
  iov[0].iov_base = &hello[0];
  iov[0].iov_len = base::ArraySize(hello);

  iov[1].iov_base = &world[0];
  iov[1].iov_len = base::ArraySize(world);

  struct msghdr hdr = {};
  hdr.msg_iov = iov;
  hdr.msg_iovlen = base::ArraySize(iov);

  UnixSocketRaw::ShiftMsgHdrPosix(
      base::ArraySize(world) + base::ArraySize(hello), &hdr);
  EXPECT_EQ(hdr.msg_iov, nullptr);
  EXPECT_EQ(static_cast<int>(hdr.msg_iovlen), 0);
}

// For use in PartialSendMsgAll template argument. Cannot be a lambda.
int RollbackSigaction(const struct sigaction* act) {
  return sigaction(SIGWINCH, act, nullptr);
}

TEST_F(UnixSocketTest, PartialSendMsgAll) {
  UnixSocketRaw send_sock;
  UnixSocketRaw recv_sock;
  std::tie(send_sock, recv_sock) =
      UnixSocketRaw::CreatePairPosix(kTestSocket.family(), SockType::kStream);
  ASSERT_TRUE(send_sock);
  ASSERT_TRUE(recv_sock);

  // Set bufsize to minimum.
  int bufsize = 1024;
  ASSERT_EQ(setsockopt(send_sock.fd(), SOL_SOCKET, SO_SNDBUF, &bufsize,
                       sizeof(bufsize)),
            0);
  ASSERT_EQ(setsockopt(recv_sock.fd(), SOL_SOCKET, SO_RCVBUF, &bufsize,
                       sizeof(bufsize)),
            0);

  // Send something larger than send + recv kernel buffers combined to make
  // sendmsg block.
  std::string send_buf(8192, '\0');
  // Make MSAN happy.
  for (size_t i = 0; i < send_buf.size(); ++i)
    send_buf[i] = static_cast<char>(i % 256);
  std::string recv_buf(send_buf.size(), '\0');

  // Need to install signal handler to cause the interrupt to happen.
  // man 3 pthread_kill:
  //   Signal dispositions are process-wide: if a signal handler is
  //   installed, the handler will be invoked in the thread thread, but if
  //   the disposition of the signal is "stop", "continue", or "terminate",
  //   this action will affect the whole process.
  struct sigaction oldact;
  struct sigaction newact = {};
  newact.sa_handler = [](int) {};
  ASSERT_EQ(sigaction(SIGWINCH, &newact, &oldact), 0);
  base::ScopedResource<const struct sigaction*, RollbackSigaction, nullptr>
      rollback(&oldact);

  auto blocked_thread = pthread_self();
  std::thread th([blocked_thread, &recv_sock, &recv_buf] {
    ssize_t rd = PERFETTO_EINTR(read(recv_sock.fd(), &recv_buf[0], 1));
    ASSERT_EQ(rd, 1);
    // We are now sure the other thread is in sendmsg, interrupt send.
    ASSERT_EQ(pthread_kill(blocked_thread, SIGWINCH), 0);
    // Drain the socket to allow SendMsgAllPosix to succeed.
    size_t offset = 1;
    while (offset < recv_buf.size()) {
      rd = PERFETTO_EINTR(
          read(recv_sock.fd(), &recv_buf[offset], recv_buf.size() - offset));
      ASSERT_GE(rd, 0);
      offset += static_cast<size_t>(rd);
    }
  });

  // Test sending the send_buf in several chunks as an iov to exercise the
  // more complicated code-paths of SendMsgAllPosix.
  struct msghdr hdr = {};
  struct iovec iov[4];
  ASSERT_EQ(send_buf.size() % base::ArraySize(iov), 0u)
      << "Cannot split buffer into even pieces.";
  const size_t kChunkSize = send_buf.size() / base::ArraySize(iov);
  for (size_t i = 0; i < base::ArraySize(iov); ++i) {
    iov[i].iov_base = &send_buf[i * kChunkSize];
    iov[i].iov_len = kChunkSize;
  }
  hdr.msg_iov = iov;
  hdr.msg_iovlen = base::ArraySize(iov);

  ASSERT_EQ(send_sock.SendMsgAllPosix(&hdr),
            static_cast<ssize_t>(send_buf.size()));
  send_sock.Shutdown();
  th.join();
  // Make sure the re-entry logic was actually triggered.
  ASSERT_EQ(hdr.msg_iov, nullptr);
  ASSERT_EQ(memcmp(&send_buf[0], &recv_buf[0], send_buf.size()), 0);
}

// Regression test for b/193234818. SO_SNDTIMEO is unreliable on most systems.
// It doesn't guarantee that the whole send() call blocks for at most X, as the
// kernel rearms the timeout if the send buffers frees up and allows a partial
// send. This test reproduces the issue 100% on Mac. Unfortunately on Linux the
// repro seem to happen only when a suspend happens in the middle.
TEST_F(UnixSocketTest, BlockingSendTimeout) {
  TestTaskRunner ttr;
  UnixSocketRaw send_sock;
  UnixSocketRaw recv_sock;
  std::tie(send_sock, recv_sock) =
      UnixSocketRaw::CreatePairPosix(kTestSocket.family(), SockType::kStream);

  auto blocking_send_done = ttr.CreateCheckpoint("blocking_send_done");

  std::thread tx_thread([&] {
    // Fill the tx buffer in non-blocking mode.
    send_sock.SetBlocking(false);
    char buf[1024 * 16]{};
    while (send_sock.Send(buf, sizeof(buf)) > 0) {
    }

    // Then do a blocking send. It should return a partial value within the tx
    // timeout.
    send_sock.SetBlocking(true);
    send_sock.SetTxTimeout(10);
    ASSERT_LT(send_sock.Send(buf, sizeof(buf)),
              static_cast<ssize_t>(sizeof(buf)));
    ttr.PostTask(blocking_send_done);
  });

  // This task needs to be slow enough so that doesn't unblock the send, but
  // fast enough so that within a blocking cycle, the send re-attempts and
  // re-arms the timeout.
  PeriodicTask read_slowly_task(&ttr);
  PeriodicTask::Args args;
  args.period_ms = 1;  // Read 1 byte every ms (1 KiB/s).
  args.task = [&] {
    char rxbuf[1]{};
    recv_sock.Receive(rxbuf, sizeof(rxbuf));
  };
  read_slowly_task.Start(args);

  ttr.RunUntilCheckpoint("blocking_send_done");
  read_slowly_task.Reset();
  tx_thread.join();
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
TEST_F(UnixSocketTest, SetsCloexec) {
  // CLOEXEC set when constructing sockets through helper:
  {
    auto raw = UnixSocketRaw::CreateMayFail(base::SockFamily::kUnix,
                                            SockType::kStream);
    int flags = fcntl(raw.fd(), F_GETFD, 0);
    EXPECT_TRUE(flags & FD_CLOEXEC);
  }
  // CLOEXEC set when creating a UnixSocketRaw out of an existing fd:
  {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int flags = fcntl(fd, F_GETFD, 0);
    EXPECT_FALSE(flags & FD_CLOEXEC);

    auto raw = UnixSocketRaw(ScopedSocketHandle(fd), base::SockFamily::kUnix,
                             SockType::kStream);
    flags = fcntl(raw.fd(), F_GETFD, 0);
    EXPECT_TRUE(flags & FD_CLOEXEC);
  }
}
#endif  // !OS_FUCHSIA

#endif  // !OS_WIN

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_MAC)

// Regression test for b/239725760.
TEST_F(UnixSocketTest, Sockaddr_FilesystemLinked) {
  TempDir tmp_dir = TempDir::Create();
  std::string sock_path = tmp_dir.path() + "/test.sock";
  auto srv = UnixSocket::Listen(sock_path, &event_listener_, &task_runner_,
                                SockFamily::kUnix, SockType::kStream);
  ASSERT_TRUE(srv && srv->is_listening());
  ASSERT_TRUE(FileExists(sock_path));

  // Create a raw socket and manually connect to that (to avoid getting affected
  // by accidental future bugs in the logic that populates struct sockaddr_un).
  auto cli = UnixSocketRaw::CreateMayFail(SockFamily::kUnix, SockType::kStream);
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  StringCopy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path));
  ASSERT_EQ(0, connect(cli.fd(), reinterpret_cast<struct sockaddr*>(&addr),
                       sizeof(addr)));
  cli.Shutdown();
  remove(sock_path.c_str());
}
#endif  // OS_LINUX || OS_ANDROID || OS_MAC

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// Regression test for b/239725760.
// Abstract sockets are not supported on Mac OS.
TEST_F(UnixSocketTest, Sockaddr_AbstractUnix) {
  StackString<128> sock_name("@perfetto_test_%d_%d", getpid(), rand() % 100000);
  auto srv =
      UnixSocket::Listen(sock_name.ToStdString(), &event_listener_,
                         &task_runner_, SockFamily::kUnix, SockType::kStream);
  ASSERT_TRUE(srv && srv->is_listening());

  auto cli = UnixSocketRaw::CreateMayFail(SockFamily::kUnix, SockType::kStream);
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  StringCopy(addr.sun_path, sock_name.c_str(), sizeof(addr.sun_path));
  addr.sun_path[0] = '\0';
  auto addr_len = static_cast<socklen_t>(
      __builtin_offsetof(sockaddr_un, sun_path) + sock_name.len());
  ASSERT_EQ(0, connect(cli.fd(), reinterpret_cast<struct sockaddr*>(&addr),
                       addr_len));
}
#endif  // OS_LINUX || OS_ANDROID

}  // namespace
}  // namespace base
}  // namespace perfetto
