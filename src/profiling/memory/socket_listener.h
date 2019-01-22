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

#ifndef SRC_PROFILING_MEMORY_SOCKET_LISTENER_H_
#define SRC_PROFILING_MEMORY_SOCKET_LISTENER_H_

#include "perfetto/base/unix_socket.h"

#include "src/profiling/memory/bookkeeping.h"
#include "src/profiling/memory/process_matcher.h"
#include "src/profiling/memory/queue_messages.h"
#include "src/profiling/memory/record_reader.h"
#include "src/profiling/memory/unwinding.h"
#include "src/profiling/memory/wire_protocol.h"

#include <map>
#include <memory>

namespace perfetto {
namespace profiling {

class SocketListener : public base::UnixSocket::EventListener,
                       public ProcessMatcher::Delegate {
 public:
  SocketListener(std::function<void(UnwindingRecord)> fn,
                 BookkeepingThread* bookkeeping_thread)
      : callback_function_(std::move(fn)),
        bookkeeping_thread_(bookkeeping_thread),
        process_matcher_(this) {}
  void OnDisconnect(base::UnixSocket* self) override;
  void OnNewIncomingConnection(
      base::UnixSocket* self,
      std::unique_ptr<base::UnixSocket> new_connection) override;
  void OnDataAvailable(base::UnixSocket* self) override;

  void Match(const Process& process,
             const std::vector<const ProcessSetSpec*>& process_sets) override;
  void Disconnect(pid_t pid) override;

  // Delegate for OnNewIncomingConnection.
  void HandleClientConnection(std::unique_ptr<base::UnixSocket> new_connection,
                              Process peer_process);

  ProcessMatcher& process_matcher() { return process_matcher_; }

 private:
  struct SocketInfo {
    SocketInfo(std::unique_ptr<base::UnixSocket> s) : sock(std::move(s)) {}

    const std::unique_ptr<base::UnixSocket> sock;
    RecordReader record_reader;
  };

  struct ProcessInfo {
    ProcessInfo(Process p);

    void Connected(ProcessMatcher* process_matcher,
                   BookkeepingThread* bookkeeping_thread);

    Process process;
    ProcessMatcher::ProcessHandle matcher_handle;
    BookkeepingThread::ProcessHandle bookkeeping_handle;
    bool connected = false;
    bool set_up = false;

    ClientConfiguration client_config{};
    std::map<base::UnixSocket*, SocketInfo> sockets;
    std::shared_ptr<UnwindingMetadata> unwinding_metadata;
  };

  void RecordReceived(base::UnixSocket*, size_t, std::unique_ptr<uint8_t[]>);

  std::map<pid_t, ProcessInfo> process_info_;
  std::function<void(UnwindingRecord)> callback_function_;
  BookkeepingThread* const bookkeeping_thread_;
  ProcessMatcher process_matcher_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_SOCKET_LISTENER_H_
