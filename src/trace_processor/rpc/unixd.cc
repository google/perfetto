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

#include "src/trace_processor/rpc/unixd.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/proc_utils.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/lock_free_task_runner.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/rpc/rpc.h"
#include "src/trace_processor/rpc/session_lifecycle.h"
#include "src/trace_processor/rpc/session_paths.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define PERFETTO_TP_UNIXD_POSIX() 1
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/scoped_file.h"
#else
#define PERFETTO_TP_UNIXD_POSIX() 0
#endif

namespace perfetto::trace_processor {
namespace {

// Wraps |value| in single quotes if it contains characters that aren't safe to
// leave bare in a shell word, so the startup record stays cut-and-pasteable.
std::string ShellQuoteIfNeeded(const std::string& value) {
  bool needs_quote = value.empty();
  for (char c : value) {
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
                c == '-' || c == '\\' || c == ':';
    if (!safe) {
      needs_quote = true;
      break;
    }
  }
  if (!needs_quote)
    return value;
  std::string out = "'";
  for (char c : value) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

// Prints the one-line, machine-parseable startup record to |f|.
void PrintStartupRecord(FILE* f,
                        int pid,
                        const std::string& session,
                        const std::string& socket_path,
                        uint32_t idle_timeout_ms) {
  std::string idle_timeout =
      idle_timeout_ms == 0 ? "never" : std::to_string(idle_timeout_ms) + "ms";
  fprintf(f,
          "perfetto-session pid=%d session=%s socket-path=%s idle-timeout=%s\n",
          pid, ShellQuoteIfNeeded(session).c_str(),
          ShellQuoteIfNeeded(socket_path).c_str(), idle_timeout.c_str());
  fflush(f);
}

class UnixRpcServer : public base::UnixSocket::EventListener {
 public:
  UnixRpcServer(Rpc& rpc, UnixServerArgs args)
      : rpc_(rpc), args_(std::move(args)) {}

  base::Status Run();

  // base::UnixSocket::EventListener implementation.
  void OnNewIncomingConnection(
      base::UnixSocket*,
      std::unique_ptr<base::UnixSocket> new_conn) override;
  void OnDisconnect(base::UnixSocket* self) override;
  void OnDataAvailable(base::UnixSocket* self) override;

 private:
  struct ClientConn {
    std::unique_ptr<base::UnixSocket> sock;
    protozero::ProtoRingBuffer rxbuf;
  };

  ClientConn* FindConn(base::UnixSocket* self);
  void DispatchMessage(base::UnixSocket* sock, const uint8_t* data, size_t len);
  void Shutdown();

  Rpc& rpc_;
  UnixServerArgs args_;
  std::string pid_path_;
  base::MaybeLockFreeTaskRunner task_runner_;
  std::unique_ptr<base::UnixSocket> listen_sock_;
  std::vector<std::unique_ptr<ClientConn>> clients_;
  std::unique_ptr<IdleReaper> reaper_;
};

// Writes the current pid to |pid_path| so `server kill` can stop the server by
// pid. Best-effort.
void WritePidFile(const std::string& pid_path, int pid) {
  if (FILE* f = fopen(pid_path.c_str(), "w")) {
    fprintf(f, "%d", pid);
    fclose(f);
  }
}

#if PERFETTO_TP_UNIXD_POSIX()
// Set so the signal handler can clean up on Ctrl-C. Only ever point at strings
// that outlive the handler (the server's socket and pid-file paths).
const char* g_socket_path_for_signal = nullptr;
const char* g_pid_path_for_signal = nullptr;

[[noreturn]] void HandleTermSignal(int) {
  // unlink() is async-signal-safe.
  if (g_socket_path_for_signal)
    unlink(g_socket_path_for_signal);
  if (g_pid_path_for_signal)
    unlink(g_pid_path_for_signal);
  _exit(0);
}

// Forks into the background. The parent prints the startup record (it knows the
// child pid) and exits; the child detaches (setsid + redirect std fds) and
// returns to keep serving. The socket is already bound before this is called,
// so the record the parent prints is accurate.
void DaemonizeAndPrintRecord(const UnixServerArgs& args) {
  base::Pipe pipe = base::Pipe::Create(base::Pipe::kBothBlock);
  pid_t pid = fork();
  PERFETTO_CHECK(pid != -1);
  if (pid > 0) {
    // Parent: wait for the child to detach, print the record, exit.
    pipe.wr.reset();
    char c = '\0';
    base::ignore_result(base::Read(*pipe.rd, &c, 1));
    PrintStartupRecord(stdout, pid, args.session_name, args.socket_path,
                       args.idle_timeout_ms);
    _exit(0);
  }
  // Child: detach from the controlling terminal and silence std fds.
  PERFETTO_CHECK(setsid() != -1);
  base::ScopedFile null = base::OpenFile("/dev/null", O_RDWR);
  if (null) {
    base::ignore_result(dup2(*null, STDIN_FILENO));
    base::ignore_result(dup2(*null, STDOUT_FILENO));
    base::ignore_result(dup2(*null, STDERR_FILENO));
  }
  base::ignore_result(base::WriteAll(*pipe.wr, "1", 1));
}
#endif  // PERFETTO_TP_UNIXD_POSIX()

base::Status UnixRpcServer::Run() {
  if (args_.daemonize && !PERFETTO_TP_UNIXD_POSIX()) {
    return base::ErrStatus("--daemonize is not supported on this platform yet");
  }

  // Clean up a socket left behind by a previous server. If something still
  // accepts a connection there, refuse to clobber it.
  if (base::FileExists(args_.socket_path)) {
    auto probe = base::UnixSocketRaw::CreateMayFail(base::SockFamily::kUnix,
                                                    base::SockType::kStream);
    if (probe && probe.Connect(args_.socket_path)) {
      return base::ErrStatus(
          "A live session already holds %s. Pick another --name/--path or stop "
          "the existing server.",
          args_.socket_path.c_str());
    }
    remove(args_.socket_path.c_str());
  }

  // Bind the listening socket up-front (before any optional fork) so that, when
  // daemonizing, the parent only prints the startup record once the socket is
  // actually bound.
  auto raw = base::UnixSocketRaw::CreateMayFail(base::SockFamily::kUnix,
                                                base::SockType::kStream);
  if (!raw || !raw.Bind(args_.socket_path) || !raw.Listen()) {
    return base::ErrStatus("Failed to bind session socket at %s",
                           args_.socket_path.c_str());
  }

  bool printed_record = false;
#if PERFETTO_TP_UNIXD_POSIX()
  if (args_.daemonize) {
    DaemonizeAndPrintRecord(args_);  // Parent exits inside; child returns.
    printed_record = true;
  }
#endif
  if (!printed_record) {
    PrintStartupRecord(stdout, static_cast<int>(base::GetProcessId()),
                       args_.session_name, args_.socket_path,
                       args_.idle_timeout_ms);
    fprintf(stderr,
            "[unix] Serving warm session '%s'. Query it with:\n"
            "  trace_processor_shell query --remote %s \"SELECT ...\"\n"
            "Stop it with Ctrl-C, or: "
            "trace_processor_shell server kill %s\n",
            args_.session_name.c_str(), args_.session_name.c_str(),
            args_.session_name.c_str());
  }

  // Record our pid next to the socket so `server kill` can stop us. This
  // runs in the serving process (the child, when daemonized), so the pid
  // matches the one printed in the startup record.
  pid_path_ = args_.socket_path + session::kPidFileSuffix;
  WritePidFile(pid_path_, static_cast<int>(base::GetProcessId()));

  listen_sock_ = base::UnixSocket::Listen(raw.ReleaseFd(), this, &task_runner_,
                                          base::SockFamily::kUnix,
                                          base::SockType::kStream);
  if (!listen_sock_ || !listen_sock_->is_listening()) {
    return base::ErrStatus("Failed to listen on session socket at %s",
                           args_.socket_path.c_str());
  }

  reaper_ =
      std::make_unique<IdleReaper>(&task_runner_, args_.idle_timeout_ms,
                                   args_.idle_start, [this] { Shutdown(); });
  reaper_->Start();

#if PERFETTO_TP_UNIXD_POSIX()
  g_socket_path_for_signal = args_.socket_path.c_str();
  g_pid_path_for_signal = pid_path_.c_str();
  signal(SIGINT, HandleTermSignal);
  signal(SIGTERM, HandleTermSignal);
#endif

  task_runner_.Run();
  remove(args_.socket_path.c_str());
  remove(pid_path_.c_str());
  return base::OkStatus();
}

void UnixRpcServer::Shutdown() {
  remove(args_.socket_path.c_str());
  remove(pid_path_.c_str());
  task_runner_.Quit();
}

void UnixRpcServer::OnNewIncomingConnection(
    base::UnixSocket*,
    std::unique_ptr<base::UnixSocket> new_conn) {
  auto conn = std::make_unique<ClientConn>();
  conn->sock = std::move(new_conn);
  clients_.push_back(std::move(conn));
}

void UnixRpcServer::OnDisconnect(base::UnixSocket* self) {
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if ((*it)->sock.get() == self) {
      clients_.erase(it);
      return;
    }
  }
}

UnixRpcServer::ClientConn* UnixRpcServer::FindConn(base::UnixSocket* self) {
  for (auto& c : clients_) {
    if (c->sock.get() == self)
      return c.get();
  }
  return nullptr;
}

void UnixRpcServer::OnDataAvailable(base::UnixSocket* self) {
  ClientConn* conn = FindConn(self);
  if (!conn)
    return;

  // Drain everything currently readable into this connection's framing buffer.
  char buf[4096];
  for (;;) {
    size_t n = self->Receive(buf, sizeof(buf));
    if (n == 0)
      break;
    conn->rxbuf.Append(buf, n);
  }

  // Forward only *whole* TraceProcessorRpc messages to the shared Rpc. Each is
  // re-wrapped in its TraceProcessorRpcStream framing so Rpc's own tokenizer
  // never sees a partial frame, even if a different connection disconnects
  // mid-message.
  for (;;) {
    auto msg = conn->rxbuf.ReadMessage();
    if (!msg.valid()) {
      if (msg.fatal_framing_error)
        self->Shutdown(/*notify=*/false);
      break;
    }
    DispatchMessage(self, msg.start, msg.len);
  }
}

void UnixRpcServer::DispatchMessage(base::UnixSocket* sock,
                                    const uint8_t* data,
                                    size_t len) {
  namespace pu = protozero::proto_utils;
  uint8_t preamble[16];
  uint8_t* preamble_end = preamble;
  preamble_end = pu::WriteVarInt(pu::MakeTagLengthDelimited(1), preamble_end);
  preamble_end = pu::WriteVarInt(len, preamble_end);

  if (reaper_)
    reaper_->set_query_in_flight(true);
  rpc_.SetRpcResponseFunction([sock](const void* resp, uint32_t resp_len) {
    if (resp == nullptr) {
      sock->Shutdown(/*notify=*/false);
      return;
    }
    sock->Send(resp, resp_len);
  });
  rpc_.OnRpcRequest(preamble, static_cast<size_t>(preamble_end - preamble));
  rpc_.OnRpcRequest(data, len);
  rpc_.SetRpcResponseFunction(nullptr);
  if (reaper_) {
    reaper_->set_query_in_flight(false);
    reaper_->OnActivity();
  }
}

}  // namespace

base::Status RunUnixRpcServer(Rpc& rpc, const UnixServerArgs& args) {
  UnixRpcServer server(rpc, args);
  return server.Run();
}

}  // namespace perfetto::trace_processor
