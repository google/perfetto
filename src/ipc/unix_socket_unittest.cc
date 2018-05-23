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

#include "src/ipc/unix_socket.h"

#include <sys/mman.h>

#include <list>
#include <thread>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/base/utils.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"

namespace perfetto {
namespace ipc {
namespace {

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Mock;

constexpr char kSocketName[] = TEST_SOCK_NAME("unix_socket_unittest");

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
  void SetUp() override { DESTROY_TEST_SOCK(kSocketName); }
  void TearDown() override { DESTROY_TEST_SOCK(kSocketName); }

  base::TestTaskRunner task_runner_;
  MockEventListener event_listener_;
};

TEST_F(UnixSocketTest, ConnectionFailureIfUnreachable) {
  auto cli = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
  ASSERT_FALSE(cli->is_connected());
  auto checkpoint = task_runner_.CreateCheckpoint("failure");
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), false))
      .WillOnce(InvokeWithoutArgs(checkpoint));
  task_runner_.RunUntilCheckpoint("failure");
}

// Both server and client should see an OnDisconnect() if the server drops
// incoming connections immediately as they are created.
TEST_F(UnixSocketTest, ConnectionImmediatelyDroppedByServer) {
  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
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
  auto cli = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(checkpoint));
  task_runner_.RunUntilCheckpoint("cli_connected");
  task_runner_.RunUntilCheckpoint("srv_did_shutdown");

  // Trying to send something will trigger the disconnection notification.
  auto cli_disconnected = task_runner_.CreateCheckpoint("cli_disconnected");
  EXPECT_CALL(event_listener_, OnDisconnect(cli.get()))
      .WillOnce(InvokeWithoutArgs(cli_disconnected));
  EXPECT_FALSE(cli->Send("whatever"));
  task_runner_.RunUntilCheckpoint("cli_disconnected");
}

TEST_F(UnixSocketTest, ClientAndServerExchangeData) {
  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());

  auto cli = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
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
  ASSERT_TRUE(cli->Send("cli>srv"));
  ASSERT_TRUE(srv_conn->Send("srv>cli"));
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
  ASSERT_FALSE(cli->Send("foo"));
  ASSERT_FALSE(srv_conn->Send("bar"));
  srv->Shutdown(true);
  task_runner_.RunUntilCheckpoint("cli_disconnected");
  task_runner_.RunUntilCheckpoint("srv_disconnected");
}

TEST_F(UnixSocketTest, ListenWithPassedFileDescriptor) {
  auto fd = UnixSocket::CreateAndBind(kSocketName);
  auto srv = UnixSocket::Listen(std::move(fd), &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());

  auto cli = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true));
  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  auto srv_disconnected = task_runner_.CreateCheckpoint("srv_disconnected");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, cli_connected, srv_disconnected](
                           UnixSocket*, UnixSocket* srv_conn) {
        // Read the EOF state.
        EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn))
            .WillOnce(
                InvokeWithoutArgs([srv_conn] { srv_conn->ReceiveString(); }));
        EXPECT_CALL(event_listener_, OnDisconnect(srv_conn))
            .WillOnce(InvokeWithoutArgs(srv_disconnected));
        cli_connected();
      }));
  task_runner_.RunUntilCheckpoint("cli_connected");
  ASSERT_TRUE(cli->is_connected());
  cli.reset();
  task_runner_.RunUntilCheckpoint("srv_disconnected");
}

// Mostly a stress tests. Connects kNumClients clients to the same server and
// tests that all can exchange data and can see the expected sequence of events.
TEST_F(UnixSocketTest, SeveralClients) {
  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());
  constexpr size_t kNumClients = 32;
  std::unique_ptr<UnixSocket> cli[kNumClients];

  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .Times(kNumClients)
      .WillRepeatedly(Invoke([this](UnixSocket*, UnixSocket* s) {
        EXPECT_CALL(event_listener_, OnDataAvailable(s))
            .WillOnce(Invoke([](UnixSocket* t) {
              ASSERT_EQ("PING", t->ReceiveString());
              ASSERT_TRUE(t->Send("PONG"));
            }));
      }));

  for (size_t i = 0; i < kNumClients; i++) {
    cli[i] = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
    EXPECT_CALL(event_listener_, OnConnect(cli[i].get(), true))
        .WillOnce(Invoke([](UnixSocket* s, bool success) {
          ASSERT_TRUE(success);
          ASSERT_TRUE(s->Send("PING"));
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

// Creates two processes. The server process creates a file and passes it over
// the socket to the client. Both processes mmap the file in shared mode and
// check that they see the same contents.
TEST_F(UnixSocketTest, SharedMemory) {
  int pipes[2];
  ASSERT_EQ(0, pipe(pipes));

  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  constexpr size_t kTmpSize = 4096;

  if (pid == 0) {
    // Child process.
    base::TempFile scoped_tmp = base::TempFile::CreateUnlinked();
    int tmp_fd = scoped_tmp.fd();
    ASSERT_FALSE(ftruncate(tmp_fd, kTmpSize));
    char* mem = reinterpret_cast<char*>(
        mmap(nullptr, kTmpSize, PROT_READ | PROT_WRITE, MAP_SHARED, tmp_fd, 0));
    ASSERT_NE(nullptr, mem);
    memcpy(mem, "shm rocks", 10);

    auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
    ASSERT_TRUE(srv->is_listening());
    // Signal the other process that it can connect.
    ASSERT_EQ(1, PERFETTO_EINTR(write(pipes[1], ".", 1)));
    auto checkpoint = task_runner_.CreateCheckpoint("change_seen_by_server");
    EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
        .WillOnce(Invoke(
            [this, tmp_fd, checkpoint, mem](UnixSocket*, UnixSocket* new_conn) {
              ASSERT_EQ(geteuid(), static_cast<uint32_t>(new_conn->peer_uid()));
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
    ASSERT_EQ(1, PERFETTO_EINTR(read(pipes[0], &sync_cmd, 1)));
    ASSERT_EQ('.', sync_cmd);
    auto cli =
        UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
    EXPECT_CALL(event_listener_, OnConnect(cli.get(), true));
    auto checkpoint = task_runner_.CreateCheckpoint("change_seen_by_client");
    EXPECT_CALL(event_listener_, OnDataAvailable(cli.get()))
        .WillOnce(Invoke([checkpoint](UnixSocket* s) {
          char msg[32];
          base::ScopedFile fd;
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
          ASSERT_TRUE(s->Send("change notify"));
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

constexpr size_t kAtomicWrites_FrameSize = 1123;
bool AtomicWrites_SendAttempt(UnixSocket* s,
                              base::TaskRunner* task_runner,
                              int num_frame) {
  char buf[kAtomicWrites_FrameSize];
  memset(buf, static_cast<char>(num_frame), sizeof(buf));
  if (s->Send(buf, sizeof(buf)))
    return true;
  task_runner->PostTask(
      std::bind(&AtomicWrites_SendAttempt, s, task_runner, num_frame));
  return false;
}

// Creates a client-server pair. The client sends continuously data to the
// server. Upon each Send() attempt, the client sends a buffer which is memset()
// with a unique number (0 to kNumFrames). We are deliberately trying to fill
// the socket output buffer, so we expect some of these Send()s to fail.
// The client is extremely aggressive and, when a Send() fails, just keeps
// re-posting it with the same unique number. The server verifies that we
// receive one and exactly one of each buffers, without any gaps or truncation.
TEST_F(UnixSocketTest, DISABLED_SendIsAtomic) {
  static constexpr int kNumFrames = 127;

  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());

  auto cli = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);

  auto all_frames_done = task_runner_.CreateCheckpoint("all_frames_done");
  std::set<int> received_iterations;
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke([this, &received_iterations, all_frames_done](
                           UnixSocket*, UnixSocket* srv_conn) {
        EXPECT_CALL(event_listener_, OnDataAvailable(srv_conn))
            .WillRepeatedly(
                Invoke([&received_iterations, all_frames_done](UnixSocket* s) {
                  char buf[kAtomicWrites_FrameSize];
                  size_t res = s->Receive(buf, sizeof(buf));
                  if (res == 0)
                    return;  // Spurious select(), could happen.
                  ASSERT_EQ(kAtomicWrites_FrameSize, res);
                  // Check that we didn't get two truncated frames.
                  for (size_t i = 0; i < sizeof(buf); i++)
                    ASSERT_EQ(buf[0], buf[i]);
                  ASSERT_EQ(0u, received_iterations.count(buf[0]));
                  received_iterations.insert(buf[0]);
                  if (received_iterations.size() == kNumFrames)
                    all_frames_done();
                }));
      }));

  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  EXPECT_CALL(event_listener_, OnConnect(cli.get(), true))
      .WillOnce(InvokeWithoutArgs(cli_connected));
  task_runner_.RunUntilCheckpoint("cli_connected");
  ASSERT_TRUE(cli->is_connected());
  ASSERT_EQ(geteuid(), static_cast<uint32_t>(cli->peer_uid()));

  bool did_requeue = false;
  for (int i = 0; i < kNumFrames; i++)
    did_requeue |= !AtomicWrites_SendAttempt(cli.get(), &task_runner_, i);

  // We expect that at least one of the kNumFrames didn't fit in the socket
  // buffer and was re-posted, otherwise this entire test would be pointless.
  ASSERT_TRUE(did_requeue);

  task_runner_.RunUntilCheckpoint("all_frames_done");
}

// Checks that the peer_uid() is retained after the client disconnects. The IPC
// layer needs to rely on this to validate messages received immediately before
// a client disconnects.
TEST_F(UnixSocketTest, PeerUidRetainedAfterDisconnect) {
  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());
  UnixSocket* srv_client_conn = nullptr;
  auto srv_connected = task_runner_.CreateCheckpoint("srv_connected");
  EXPECT_CALL(event_listener_, OnNewIncomingConnection(srv.get(), _))
      .WillOnce(Invoke(
          [&srv_client_conn, srv_connected](UnixSocket*, UnixSocket* srv_conn) {
            srv_client_conn = srv_conn;
            EXPECT_EQ(geteuid(), static_cast<uint32_t>(srv_conn->peer_uid()));
            srv_connected();
          }));
  auto cli_connected = task_runner_.CreateCheckpoint("cli_connected");
  auto cli = UnixSocket::Connect(kSocketName, &event_listener_, &task_runner_);
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
  EXPECT_EQ(geteuid(), static_cast<uint32_t>(srv_client_conn->peer_uid()));
}

TEST_F(UnixSocketTest, BlockingSend) {
  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());

  auto all_frames_done = task_runner_.CreateCheckpoint("all_frames_done");
  size_t total_bytes_received = 0;
  constexpr size_t kTotalBytes = 1024 * 1024 * 4;
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
  const int kTimeoutMs = 60000 * 3;

  // Perform the blocking send form another thread.
  std::thread tx_thread([] {
    base::TestTaskRunner tx_task_runner;
    MockEventListener tx_events;
    auto cli = UnixSocket::Connect(kSocketName, &tx_events, &tx_task_runner);

    auto cli_connected = tx_task_runner.CreateCheckpoint("cli_connected");
    EXPECT_CALL(tx_events, OnConnect(cli.get(), true))
        .WillOnce(InvokeWithoutArgs(cli_connected));
    tx_task_runner.RunUntilCheckpoint("cli_connected");

    auto all_sent = tx_task_runner.CreateCheckpoint("all_sent");
    char buf[1024 * 32] = {};
    tx_task_runner.PostTask([&cli, &buf, all_sent] {
      for (size_t i = 0; i < kTotalBytes / sizeof(buf); i++)
        cli->Send(buf, sizeof(buf), -1 /*fd*/,
                  UnixSocket::BlockingMode::kBlocking);
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
  auto srv = UnixSocket::Listen(kSocketName, &event_listener_, &task_runner_);
  ASSERT_TRUE(srv->is_listening());
  const int kTimeoutMs = 30000;

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
    base::TestTaskRunner tx_task_runner;
    MockEventListener tx_events;
    auto cli = UnixSocket::Connect(kSocketName, &tx_events, &tx_task_runner);

    auto cli_connected = tx_task_runner.CreateCheckpoint("cli_connected");
    EXPECT_CALL(tx_events, OnConnect(cli.get(), true))
        .WillOnce(InvokeWithoutArgs(cli_connected));
    tx_task_runner.RunUntilCheckpoint("cli_connected");

    auto send_done = tx_task_runner.CreateCheckpoint("send_done");
    // We need a
    static constexpr size_t kBufSize = 32 * 1024 * 1024;
    std::unique_ptr<char[]> buf(new char[kBufSize]());
    tx_task_runner.PostTask([&cli, &buf, send_done] {
      bool send_res = cli->Send(buf.get(), kBufSize, -1 /*fd*/,
                                UnixSocket::BlockingMode::kBlocking);
      ASSERT_FALSE(send_res);
      send_done();
    });

    tx_task_runner.RunUntilCheckpoint("send_done", kTimeoutMs);
  });
  task_runner_.RunUntilCheckpoint("receive_done", kTimeoutMs);
  tx_thread.join();
}

// TODO(primiano): add a test to check that in the case of a peer sending a fd
// and the other end just doing a recv (without taking it), the fd is closed and
// not left around.

// TODO(primiano); add a test to check that a socket can be reused after
// Shutdown(),

// TODO(primiano): add a test to check that OnDisconnect() is called in all
// possible cases.

// TODO(primiano): add tests that destroy the socket in all possible stages and
// verify that no spurious EventListener callback is received.

}  // namespace
}  // namespace ipc
}  // namespace perfetto
