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
#include "src/profiling/memory/proc_utils.h"

namespace perfetto {
namespace profiling {
namespace {

ClientConfiguration MergeProcessSetSpecs(
    const std::vector<const ProcessSetSpec*>& process_sets) {
  ClientConfiguration result{};
  for (const ProcessSetSpec* process_set : process_sets) {
    const ClientConfiguration& cfg = process_set->client_configuration;
    if (result.interval == 0 || result.interval > cfg.interval)
      result.interval = cfg.interval;
  }
  PERFETTO_DCHECK(result.interval > 0);
  if (result.interval < 1)
    result.interval = 1;
  return result;
}

}  // namespace

SocketListener::ProcessInfo::ProcessInfo(pid_t pid) {
  process.pid = pid;
  if (!GetCmdlineForPID(pid, &process.cmdline))
    PERFETTO_ELOG("Failed to get cmdline for %d", pid);
}

void SocketListener::ProcessInfo::Connected(
    ProcessMatcher* process_matcher,
    BookkeepingThread* bookkeeping_thread) {
  if (!connected) {
    matcher_handle = process_matcher->ProcessConnected(process);
    bookkeeping_handle =
        bookkeeping_thread->NotifyProcessConnected(process.pid);
  }
  connected = true;
}

void SocketListener::OnDisconnect(base::UnixSocket* self) {
  pid_t peer_pid = self->peer_pid();
  Disconnect(peer_pid);
}

void SocketListener::Disconnect(pid_t pid) {
  process_info_.erase(pid);
}

void SocketListener::Match(
    const Process& process,
    const std::vector<const ProcessSetSpec*>& process_sets) {
  pid_t pid = process.pid;
  auto process_info_it = process_info_.find(pid);
  if (process_info_it == process_info_.end()) {
    PERFETTO_DFATAL("This should not happen.");
    return;
  }

  ProcessInfo& process_info = process_info_it->second;
  if (process_info.set_up) {
    // TODO(fmayer): Allow to change sampling rate.
    return;
  }

  ClientConfiguration cfg = MergeProcessSetSpecs(process_sets);
  for (auto& raw_sock_and_sockinfo : process_info.sockets) {
    SocketInfo& sock_info = raw_sock_and_sockinfo.second;
    // TODO(fmayer): Send on one and poll(2) on the other end.
    sock_info.sock->Send(&cfg, sizeof(cfg), -1,
                         base::UnixSocket::BlockingMode::kBlocking);
  }
  process_info.client_config = std::move(cfg);
  process_info.set_up = true;
}

void SocketListener::OnNewIncomingConnection(
    base::UnixSocket*,
    std::unique_ptr<base::UnixSocket> new_connection) {
  pid_t peer_pid = new_connection->peer_pid();
  base::UnixSocket* new_connection_raw = new_connection.get();

  decltype(process_info_)::iterator it;
  std::tie(it, std::ignore) = process_info_.emplace(peer_pid, peer_pid);
  ProcessInfo& process_info = it->second;
  process_info.Connected(&process_matcher_, bookkeeping_thread_);
  process_info.sockets.emplace(new_connection_raw, std::move(new_connection));
  if (process_info.set_up) {
    new_connection_raw->Send(&process_info.client_config,
                             sizeof(process_info.client_config), -1,
                             base::UnixSocket::BlockingMode::kBlocking);
  }
}

void SocketListener::OnDataAvailable(base::UnixSocket* self) {
  pid_t peer_pid = self->peer_pid();

  auto process_info_it = process_info_.find(peer_pid);
  if (process_info_it == process_info_.end()) {
    PERFETTO_DFATAL("This should not happen.");
    return;
  }
  ProcessInfo& process_info = process_info_it->second;

  auto socket_it = process_info.sockets.find(self);
  if (socket_it == process_info.sockets.end()) {
    PERFETTO_DFATAL("Unexpected data received.");
    return;
  }
  SocketInfo& socket_info = socket_it->second;

  RecordReader::ReceiveBuffer buf = socket_info.record_reader.BeginReceive();

  size_t rd;
  if (PERFETTO_LIKELY(process_info.unwinding_metadata)) {
    rd = self->Receive(buf.data, buf.size);
  } else {
    base::ScopedFile fds[2];
    rd = self->Receive(buf.data, buf.size, fds, base::ArraySize(fds));
    if (fds[0] && fds[1]) {
      PERFETTO_DLOG("%d: Received FDs.", peer_pid);
      process_info.unwinding_metadata = std::make_shared<UnwindingMetadata>(
          peer_pid, std::move(fds[0]), std::move(fds[1]));
    } else if (fds[0] || fds[1]) {
      PERFETTO_DLOG("%d: Received partial FDs.", peer_pid);
    } else {
      PERFETTO_DLOG("%d: Received no FDs.", peer_pid);
    }
  }

  RecordReader::Record record;
  auto status = socket_info.record_reader.EndReceive(rd, &record);
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

void SocketListener::RecordReceived(base::UnixSocket* self,
                                    size_t size,
                                    std::unique_ptr<uint8_t[]> buf) {
  pid_t peer_pid = self->peer_pid();

  if (size == 0) {
    PERFETTO_DLOG("Dropping empty record.");
    return;
  }

  auto it = process_info_.find(peer_pid);
  if (it == process_info_.end()) {
    return;
  }
  ProcessInfo& process_info = it->second;

  // This needs to be a weak_ptr for two reasons:
  // 1) most importantly, the weak_ptr in unwinding_metadata_ should expire as
  // soon as the last socket for a process goes away. Otherwise, a recycled
  // PID might reuse incorrect metadata.
  // 2) it is a waste to unwind for a process that had already gone away.
  std::weak_ptr<UnwindingMetadata> weak_metadata(
      process_info.unwinding_metadata);
  callback_function_(
      {peer_pid, size, std::move(buf), std::move(weak_metadata)});
}

}  // namespace profiling
}  // namespace perfetto
