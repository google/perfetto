/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_FOR_TESTING_H_
#define SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_FOR_TESTING_H_

#include <functional>
#include <utility>

#include "src/tracing/ipc/producer/producer_ipc_client_impl.h"

namespace perfetto::test {

// Reaches private ProducerIPCClientImpl methods for integration tests.
class ProducerIPCClientTestPeer {
 public:
  // Sends a ring buffer that the test built by hand. The test owns the
  // descriptor and the mapping. The mapping must outlive all ring buffer
  // views and writers that use it.
  static void ShareRingBuffer(ProducerIPCClientImpl* client,
                              int fd,
                              uint32_t chunk_size_bytes,
                              std::function<void(bool)> callback) {
    client->ShareRingBuffer(fd, chunk_size_bytes, std::move(callback));
  }

  // Starts the disconnect that a protocol error in an IPC handler starts.
  static void ScheduleDisconnect(ProducerIPCClientImpl* client) {
    client->ScheduleDisconnect();
  }
};

}  // namespace perfetto::test

#endif  // SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_FOR_TESTING_H_
