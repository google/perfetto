/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/socket_listener.h"

#include "perfetto/base/scoped_file.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

using ::testing::_;
using ::testing::InvokeWithoutArgs;

constexpr char kSocketName[] = TEST_SOCK_NAME("socket_listener_unittest");

class SocketListenerTest : public ::testing::Test {
 protected:
  void SetUp() override { DESTROY_TEST_SOCK(kSocketName); }
  void TearDown() override { DESTROY_TEST_SOCK(kSocketName); }
};

class MockEventListener : public ipc::UnixSocket::EventListener {
 public:
  MOCK_METHOD2(OnConnect, void(ipc::UnixSocket*, bool));
};

TEST_F(SocketListenerTest, ReceiveRecord) {
  base::TestTaskRunner task_runner;
  auto callback_called = task_runner.CreateCheckpoint("callback.called");
  auto connected = task_runner.CreateCheckpoint("connected");
  auto callback_fn = [&callback_called](
                         size_t size, std::unique_ptr<uint8_t[]> buf,
                         std::weak_ptr<ProcessMetadata> metadata) {
    ASSERT_EQ(size, 1u);
    ASSERT_EQ(buf[0], '1');
    ASSERT_FALSE(metadata.expired());
    callback_called();
  };

  SocketListener listener(std::move(callback_fn));
  MockEventListener client_listener;
  EXPECT_CALL(client_listener, OnConnect(_, _))
      .WillOnce(InvokeWithoutArgs(connected));

  std::unique_ptr<ipc::UnixSocket> recv_socket =
      ipc::UnixSocket::Listen(kSocketName, &listener, &task_runner);

  std::unique_ptr<ipc::UnixSocket> client_socket =
      ipc::UnixSocket::Connect(kSocketName, &client_listener, &task_runner);

  task_runner.RunUntilCheckpoint("connected");
  uint64_t size = 1;
  base::ScopedFile fds[2] = {base::ScopedFile(open("/dev/null", O_RDONLY)),
                             base::ScopedFile(open("/dev/null", O_RDONLY))};
  int raw_fds[2] = {*fds[0], *fds[1]};
  ASSERT_TRUE(client_socket->Send(&size, sizeof(size), raw_fds,
                                  base::ArraySize(raw_fds)));
  ASSERT_TRUE(client_socket->Send("1", 1));

  task_runner.RunUntilCheckpoint("callback.called");
}

}  // namespace
}  // namespace perfetto
