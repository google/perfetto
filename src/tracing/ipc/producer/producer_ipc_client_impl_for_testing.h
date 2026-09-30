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

#include <utility>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "src/tracing/ipc/producer/producer_ipc_client_impl.h"

namespace perfetto::test {

// Reaches private ProducerIPCClientImpl state for integration tests.
class ProducerIPCClientTestPeer {
 public:
  static const base::FlatSet<ProtocolAbiVersion>& protocol_abi_versions(
      const ProducerIPCClientImpl* client) {
    return client->protocol_abi_versions_;
  }

  // Acts like an InitializeConnection reply to |offered_versions|.
  static void OnConnectionInitialized(
      ProducerIPCClientImpl* client,
      const std::vector<ProtocolAbiVersion>& offered_versions,
      const std::vector<ProtocolAbiVersion>& protocol_abi_versions,
      bool use_shmem_emulation = false,
      bool connection_succeeded = true) {
    ipc::AsyncResult<protos::gen::InitializeConnectionResponse> response;
    if (connection_succeeded) {
      response =
          ipc::AsyncResult<protos::gen::InitializeConnectionResponse>::Create();
      response->set_direct_smb_patching_supported(true);
      response->set_use_shmem_emulation(use_shmem_emulation);
      for (auto version : protocol_abi_versions) {
        response->add_protocol_abi_versions(
            static_cast<protos::gen::ProtocolAbiVersion>(version));
      }
    }
    client->OnConnectionInitialized(offered_versions, std::move(response));
  }
};

}  // namespace perfetto::test

#endif  // SRC_TRACING_IPC_PRODUCER_PRODUCER_IPC_CLIENT_IMPL_FOR_TESTING_H_
