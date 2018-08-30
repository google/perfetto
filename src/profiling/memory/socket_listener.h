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

#include "src/ipc/unix_socket.h"
#include "src/profiling/memory/record_reader.h"
#include "src/profiling/memory/unwinding.h"

#include <map>
#include <memory>

namespace perfetto {

class SocketListener : public ipc::UnixSocket::EventListener {
 public:
  SocketListener(std::function<void(size_t,
                                    std::unique_ptr<uint8_t[]>,
                                    std::weak_ptr<ProcessMetadata>)> fn)
      : callback_function_(std::move(fn)) {}
  void OnDisconnect(ipc::UnixSocket* self) override;
  void OnNewIncomingConnection(
      ipc::UnixSocket* self,
      std::unique_ptr<ipc::UnixSocket> new_connection) override;
  void OnDataAvailable(ipc::UnixSocket* self) override;

 private:
  struct Entry {
    Entry(std::unique_ptr<ipc::UnixSocket> s) : sock(std::move(s)) {}
    // Only here for ownership of the object.
    const std::unique_ptr<ipc::UnixSocket> sock;
    RecordReader record_reader;
    bool recv_fds = false;
    // The sockets own the metadata for a particular PID. When the last socket
    // for a PID disconnects, the metadata is destroyed. The unwinding threads
    // get a weak_ptr, which will be invalidated so we do not unwind for
    // processes that have already gone away.
    //
    // This does not get initialized in the ctor because the file descriptors
    // only get received after the first Receive call of the socket.
    std::shared_ptr<ProcessMetadata> process_metadata;
  };

  void RecordReceived(ipc::UnixSocket*, size_t, std::unique_ptr<uint8_t[]>);
  void InitProcess(Entry* entry,
                   pid_t peer_pid,
                   base::ScopedFile maps_fd,
                   base::ScopedFile mem_fd);

  std::map<ipc::UnixSocket*, Entry> sockets_;
  std::map<pid_t, std::weak_ptr<ProcessMetadata>> process_metadata_;
  std::function<
      void(size_t, std::unique_ptr<uint8_t[]>, std::weak_ptr<ProcessMetadata>)>
      callback_function_;
};

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_SOCKET_LISTENER_H_
