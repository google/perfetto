/*
 * Copyright (C) 2020 The Android Open Source Project
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
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/heap_profile.h"
#include "src/profiling/memory/heap_profile_internal.h"

#include "src/profiling/memory/client.h"
#include "src/profiling/memory/client_api_factory.h"
#include "src/profiling/memory/shared_ring_buffer.h"
#include "src/profiling/memory/wire_protocol.h"
#include "test/gtest_and_gmock.h"

#include <memory>

namespace perfetto {
namespace profiling {

namespace {

ClientConfiguration g_client_config;
int g_shmem_fd;

base::UnixSocketRaw& GlobalServerSocket() {
  static base::UnixSocketRaw* srv_sock = new base::UnixSocketRaw;
  return *srv_sock;
}

void DisconnectGlobalServerSocket() {
  base::UnixSocketRaw destroy;
  std::swap(destroy, GlobalServerSocket());
}

}  // namespace

// This is called by AHeapProfile_initSession (client_api.cc) to construct a
// client. The Client API requires to be linked against another compliation
// unit that provides this function. This way, it can be used in different
// circumstances (central heapprofd, fork heapprofd) and be agnostic about the
// details. This is is used to create a test Client here.
void StartHeapprofdIfStatic() {}
std::shared_ptr<Client> ConstructClient(
    UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator) {
  base::UnixSocketRaw cli_sock;
  base::UnixSocketRaw& srv_sock = GlobalServerSocket();
  std::tie(cli_sock, srv_sock) = base::UnixSocketRaw::CreatePairPosix(
      base::SockFamily::kUnix, base::SockType::kStream);
  auto ringbuf = SharedRingBuffer::Create(8 * 1048576);
  PERFETTO_CHECK(ringbuf);
  PERFETTO_CHECK(cli_sock);
  PERFETTO_CHECK(srv_sock);
  g_shmem_fd = ringbuf->fd();
  return std::allocate_shared<Client>(unhooked_allocator, std::move(cli_sock),
                                      g_client_config, std::move(*ringbuf),
                                      getpid(), GetMainThreadStackRange());
}

namespace {

TEST(ClientApiTest, NoClient) {
  uint32_t heap_id = AHeapProfile_registerHeap(AHeapInfo_create("NoClient"));
  EXPECT_FALSE(AHeapProfile_reportAllocation(heap_id, 1, 1));
}

TEST(ClientApiTest, ClientEnabledHeap) {
  uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_create("ClientEnabledHeap"));
  ClientConfiguration client_config{};
  client_config.default_interval = 1;
  strcpy(&client_config.heaps[0].name[0], "ClientEnabledHeap");
  client_config.heaps[0].interval = 1;
  client_config.num_heaps = 1;

  g_client_config = client_config;

  AHeapProfile_initSession(malloc, free);
  PERFETTO_CHECK(g_shmem_fd);
  auto ringbuf = SharedRingBuffer::Attach(base::ScopedFile(dup(g_shmem_fd)));
  g_shmem_fd = 0;
  PERFETTO_CHECK(ringbuf);
  EXPECT_TRUE(AHeapProfile_reportAllocation(heap_id, 1, 1));
  // Check that the service received something on the shmem.
  EXPECT_TRUE(ringbuf->BeginRead());
  DisconnectGlobalServerSocket();
  ringbuf->SetShuttingDown();
  EXPECT_FALSE(AHeapProfile_reportAllocation(heap_id, 1, 1));
}

TEST(ClientApiTest, ClientAllHeaps) {
  uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_create("ClientAllHeaps"));
  ClientConfiguration client_config{};
  client_config.default_interval = 1;
  client_config.all_heaps = true;

  g_client_config = client_config;

  AHeapProfile_initSession(malloc, free);
  PERFETTO_CHECK(g_shmem_fd);
  auto ringbuf = SharedRingBuffer::Attach(base::ScopedFile(dup(g_shmem_fd)));
  g_shmem_fd = 0;
  PERFETTO_CHECK(ringbuf);
  EXPECT_TRUE(AHeapProfile_reportAllocation(heap_id, 1, 1));
  // Check that the service received something on the shmem.
  EXPECT_TRUE(ringbuf->BeginRead());
  DisconnectGlobalServerSocket();
  ringbuf->SetShuttingDown();
  EXPECT_FALSE(AHeapProfile_reportAllocation(heap_id, 1, 1));
}

}  // namespace

}  // namespace profiling
}  // namespace perfetto
