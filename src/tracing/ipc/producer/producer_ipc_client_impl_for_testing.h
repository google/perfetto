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

// Calls the production offer sender. The test owns the descriptor and mapping.
// The mapping must outlive all ring views and writers that borrow it.
class ProducerIPCClientOfferForTest {
 public:
  static void OfferRingBuffer(ProducerIPCClientImpl* client,
                              int fd,
                              uint32_t chunk_size_bytes,
                              std::function<void(bool)> callback) {
    client->OfferRingBuffer(fd, chunk_size_bytes, std::move(callback));
  }
};

}  // namespace perfetto::test

#endif  // SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_FOR_TESTING_H_
