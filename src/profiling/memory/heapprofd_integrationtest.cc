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

#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"
#include "src/profiling/memory/client.h"
#include "src/profiling/memory/socket_listener.h"
#include "src/profiling/memory/unwinding.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

constexpr char kSocketName[] = TEST_SOCK_NAME("heapprofd_integrationtest");

void __attribute__((noinline)) OtherFunction(Client* client) {
  client->RecordMalloc(10, 0xf00);
}

void __attribute__((noinline)) SomeFunction(Client* client) {
  OtherFunction(client);
}

class HeapprofdIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override { DESTROY_TEST_SOCK(kSocketName); }
  void TearDown() override { DESTROY_TEST_SOCK(kSocketName); }
};

// TODO(fmayer): Fix out of tree integration test.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_EndToEnd EndToEnd
#else
#define MAYBE_EndToEnd DISABLED_EndToEnd
#endif

TEST_F(HeapprofdIntegrationTest, MAYBE_EndToEnd) {
  GlobalCallstackTrie callsites;

  base::TestTaskRunner task_runner;
  auto done = task_runner.CreateCheckpoint("done");
  SocketListener listener(
      [&done](UnwindingRecord r) {
        // TODO(fmayer): Test symbolization and result of unwinding.
        BookkeepingRecord bookkeeping_record;
        ASSERT_TRUE(HandleUnwindingRecord(&r, &bookkeeping_record));
        HandleBookkeepingRecord(&bookkeeping_record);
        done();
      },
      &callsites);

  auto sock = base::UnixSocket::Listen(kSocketName, &listener, &task_runner);
  if (!sock->is_listening()) {
    PERFETTO_ELOG("Socket not listening.");
    PERFETTO_CHECK(false);
  }
  Client client(kSocketName, 1);
  SomeFunction(&client);
  task_runner.RunUntilCheckpoint("done");
}

}  // namespace
}  // namespace perfetto
