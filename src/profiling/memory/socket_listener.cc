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
#include "perfetto/base/utils.h"

namespace perfetto {
namespace profiling {

void SocketListener::OnDisconnect(base::UnixSocket* self) {
  bookkeeping_thread_->NotifyClientDisconnected(self->peer_pid());
  auto it = process_info_.find(self->peer_pid());
  if (it != process_info_.end()) {
    ProcessInfo& process_info = it->second;
    process_info.sockets.erase(self);
  } else {
    PERFETTO_DFATAL("Disconnect from socket without ProcessInfo.");
  }
  sockets_.erase(self);
}

void SocketListener::OnNewIncomingConnection(
    base::UnixSocket*,
    std::unique_ptr<base::UnixSocket> new_connection) {
  base::UnixSocket* new_connection_raw = new_connection.get();
  pid_t pid = new_connection_raw->peer_pid();

  auto it = process_info_.find(pid);
  if (it == process_info_.end()) {
    PERFETTO_DFATAL("Unexpected connection.");
    return;
  }
  ProcessInfo& process_info = it->second;

  sockets_.emplace(new_connection_raw, std::move(new_connection));
  process_info.sockets.emplace(new_connection_raw);
  // TODO(fmayer): Move destruction of bookkeeping data to
  // HeapprofdProducer.
  bookkeeping_thread_->NotifyClientConnected(pid);
}

void SocketListener::OnDataAvailable(base::UnixSocket* self) {
  auto socket_it = sockets_.find(self);
  if (socket_it == sockets_.end())
    return;

  pid_t peer_pid = self->peer_pid();

  Entry& entry = socket_it->second;
  RecordReader::ReceiveBuffer buf = entry.record_reader.BeginReceive();

  auto process_info_it = process_info_.find(peer_pid);
  if (process_info_it == process_info_.end()) {
    PERFETTO_DFATAL("This should not happen.");
    return;
  }
  ProcessInfo& process_info = process_info_it->second;

  size_t rd;
  if (PERFETTO_LIKELY(entry.recv_fds)) {
    rd = self->Receive(buf.data, buf.size);
  } else {
    auto it = unwinding_metadata_.find(peer_pid);
    if (it != unwinding_metadata_.end() && !it->second.expired()) {
      entry.recv_fds = true;
      // If the process already has metadata, this is an additional socket for
      // an existing process. Reuse existing metadata and close the received
      // file descriptors.
      entry.unwinding_metadata = std::shared_ptr<UnwindingMetadata>(it->second);
      rd = self->Receive(buf.data, buf.size);
    } else {
      base::ScopedFile fds[2];
      rd = self->Receive(buf.data, buf.size, fds, base::ArraySize(fds));
      if (fds[0] && fds[1]) {
        PERFETTO_DLOG("%d: Received FDs.", peer_pid);
        entry.recv_fds = true;
        entry.unwinding_metadata = std::make_shared<UnwindingMetadata>(
            peer_pid, std::move(fds[0]), std::move(fds[1]));
        unwinding_metadata_[peer_pid] = entry.unwinding_metadata;
        self->Send(&process_info.client_config,
                   sizeof(process_info.client_config), -1,
                   base::UnixSocket::BlockingMode::kBlocking);
      } else if (fds[0] || fds[1]) {
        PERFETTO_DLOG("%d: Received partial FDs.", peer_pid);
      } else {
        PERFETTO_DLOG("%d: Received no FDs.", peer_pid);
      }
    }
  }
  RecordReader::Record record;
  auto status = entry.record_reader.EndReceive(rd, &record);
  switch (status) {
    case (RecordReader::Result::Noop):
      break;
    case (RecordReader::Result::RecordReceived):
      RecordReceived(self, static_cast<size_t>(record.size),
                     std::move(record.data));
      break;
    case (RecordReader::Result::KillConnection):
      self->Shutdown(true);
      break;
  }
}

SocketListener::ProfilingSession SocketListener::ExpectPID(
    pid_t pid,
    ClientConfiguration cfg) {
  PERFETTO_DLOG("Expecting connection from %d", pid);
  bool inserted;
  std::tie(std::ignore, inserted) = process_info_.emplace(pid, std::move(cfg));
  if (!inserted)
    return ProfilingSession(0, nullptr);
  return ProfilingSession(pid, this);
}

void SocketListener::ShutdownPID(pid_t pid) {
  PERFETTO_DLOG("Shutting down connecting from %d", pid);
  auto it = process_info_.find(pid);
  if (it == process_info_.end()) {
    PERFETTO_DFATAL("Shutting down nonexistant pid.");
    return;
  }
  ProcessInfo& process_info = it->second;
  // Disconnect all sockets for process.
  for (base::UnixSocket* socket : process_info.sockets)
    socket->Shutdown(true);
}

void SocketListener::RecordReceived(base::UnixSocket* self,
                                    size_t size,
                                    std::unique_ptr<uint8_t[]> buf) {
  auto it = sockets_.find(self);
  if (it == sockets_.end()) {
    // This happens for zero-length records, because the callback gets called
    // in the first call to Read. Because zero length records are useless,
    // this is not a problem.
    return;
  }
  Entry& entry = it->second;
  if (!entry.unwinding_metadata) {
    PERFETTO_DLOG("Received record without process metadata.");
    return;
  }

  if (size == 0) {
    PERFETTO_DLOG("Dropping empty record.");
    return;
  }
  // This needs to be a weak_ptr for two reasons:
  // 1) most importantly, the weak_ptr in unwinding_metadata_ should expire as
  // soon as the last socket for a process goes away. Otherwise, a recycled
  // PID might reuse incorrect metadata.
  // 2) it is a waste to unwind for a process that had already gone away.
  std::weak_ptr<UnwindingMetadata> weak_metadata(entry.unwinding_metadata);
  callback_function_({entry.unwinding_metadata->pid, size, std::move(buf),
                      std::move(weak_metadata)});
}

}  // namespace profiling
}  // namespace perfetto
