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

#ifndef INCLUDE_PERFETTO_IPC_CLIENT_INFO_H_
#define INCLUDE_PERFETTO_IPC_CLIENT_INFO_H_

#include "perfetto/base/logging.h"
#include "perfetto/ipc/basic_types.h"

namespace perfetto {
namespace ipc {

// Passed to Service(s) to identify remote clients.
class ClientInfo {
 public:
  ClientInfo() = default;
  ClientInfo(ClientID client_id, int uid) : client_id_(client_id), uid_(uid) {}

  bool operator==(const ClientInfo& other) const {
    return (client_id_ == other.client_id_ && uid_ == other.uid_);
  }
  bool operator!=(const ClientInfo& other) const { return !(*this == other); }

  // For map<> and other sorted containers.
  bool operator<(const ClientInfo& other) const {
    PERFETTO_DCHECK(client_id_ != other.client_id_ || *this == other);
    return client_id_ < other.client_id_;
  }

  bool is_valid() const { return client_id_ != 0; }

  // A monotonic counter.
  ClientID client_id() const { return client_id_; }

  // Posix User ID. Comes from the kernel, can be trusted.
  int uid() const { return uid_; }

 private:
  ClientID client_id_ = 0;
  int uid_ = -1;
};

}  // namespace ipc
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_IPC_CLIENT_INFO_H_
