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
#include "src/profiling/memory/queue_messages.h"
#include "src/profiling/memory/record_reader.h"
#include "src/profiling/memory/unwinding.h"
#include "src/profiling/memory/wire_protocol.h"

#include <map>
#include <memory>

namespace perfetto {
namespace profiling {

class SocketListener : public base::UnixSocket::EventListener {
 public:
  friend class ProfilingSession;
  class ProfilingSession {
   public:
    friend class SocketListener;

    ProfilingSession(ProfilingSession&& other)
        : pid_(other.pid_), listener_(other.listener_) {
      other.listener_ = nullptr;
    }

    ~ProfilingSession() {
      if (listener_)
        listener_->ShutdownPID(pid_);
    }
    ProfilingSession& operator=(ProfilingSession&& other) {
      pid_ = other.pid_;
      listener_ = other.listener_;
      other.listener_ = nullptr;
      return *this;
    }

    operator bool() const { return listener_ != nullptr; }

    ProfilingSession(const ProfilingSession&) = delete;
    ProfilingSession& operator=(const ProfilingSession&) = delete;

   private:
    ProfilingSession(pid_t pid, SocketListener* listener)
        : pid_(pid), listener_(listener) {}

    pid_t pid_;
    SocketListener* listener_ = nullptr;
  };

  SocketListener(std::function<void(UnwindingRecord)> fn,
                 BookkeepingThread* bookkeeping_thread)
      : callback_function_(std::move(fn)),
        bookkeeping_thread_(bookkeeping_thread) {}
  void OnDisconnect(base::UnixSocket* self) override;
  void OnNewIncomingConnection(
      base::UnixSocket* self,
      std::unique_ptr<base::UnixSocket> new_connection) override;
  void OnDataAvailable(base::UnixSocket* self) override;

  ProfilingSession ExpectPID(pid_t pid, ClientConfiguration cfg);

 private:
  struct ProcessInfo {
    ProcessInfo(ClientConfiguration cfg) : client_config(std::move(cfg)) {}
    ClientConfiguration client_config;
    std::set<base::UnixSocket*> sockets;
  };

  struct Entry {
    Entry(std::unique_ptr<base::UnixSocket> s) : sock(std::move(s)) {}
    // Only here for ownership of the object.
    const std::unique_ptr<base::UnixSocket> sock;
    RecordReader record_reader;
    bool recv_fds = false;
    // The sockets own the metadata for a particular PID. When the last socket
    // for a PID disconnects, the metadata is destroyed. The unwinding threads
    // get a weak_ptr, which will be invalidated so we do not unwind for
    // processes that have already gone away.
    //
    // This does not get initialized in the ctor because the file descriptors
    // only get received after the first Receive call of the socket.
    std::shared_ptr<UnwindingMetadata> unwinding_metadata;
  };

  void RecordReceived(base::UnixSocket*, size_t, std::unique_ptr<uint8_t[]>);
  void ShutdownPID(pid_t pid);

  std::map<base::UnixSocket*, Entry> sockets_;
  std::map<pid_t, std::weak_ptr<UnwindingMetadata>> unwinding_metadata_;
  std::map<pid_t, ProcessInfo> process_info_;
  std::function<void(UnwindingRecord)> callback_function_;
  BookkeepingThread* const bookkeeping_thread_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_SOCKET_LISTENER_H_
