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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define PERFETTO_TP_UNIXD_HAS_SIGNALS() 1
#include <signal.h>
#include <unistd.h>
#else
#define PERFETTO_TP_UNIXD_HAS_SIGNALS() 0
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

  Rpc& rpc_;
  UnixServerArgs args_;
  base::MaybeLockFreeTaskRunner task_runner_;
  std::unique_ptr<base::UnixSocket> listen_sock_;
  std::vector<std::unique_ptr<ClientConn>> clients_;
};

#if PERFETTO_TP_UNIXD_HAS_SIGNALS()
// Set so the signal handler can unlink the socket on Ctrl-C. Only ever points
// at a string that outlives the handler (the server's socket path).
const char* g_socket_path_for_signal = nullptr;

[[noreturn]] void HandleTermSignal(int) {
  if (g_socket_path_for_signal)
    unlink(g_socket_path_for_signal);  // async-signal-safe.
  _exit(0);
}
#endif

base::Status UnixRpcServer::Run() {
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

  listen_sock_ = base::UnixSocket::Listen(
      args_.socket_path, this, &task_runner_, base::SockFamily::kUnix,
      base::SockType::kStream);
  if (!listen_sock_ || !listen_sock_->is_listening()) {
    return base::ErrStatus("Failed to bind session socket at %s",
                           args_.socket_path.c_str());
  }

  // Startup record on stdout (machine-parseable, one line). Human guidance goes
  // to stderr so stdout stays clean for tooling.
  printf("perfetto-session pid=%d session=%s socket-path=%s\n",
         static_cast<int>(base::GetProcessId()),
         ShellQuoteIfNeeded(args_.session_name).c_str(),
         ShellQuoteIfNeeded(args_.socket_path).c_str());
  fflush(stdout);
  fprintf(stderr,
          "[unix] Serving warm session '%s'. Query it with:\n"
          "  trace_processor_shell query --remote %s \"SELECT ...\"\n"
          "Press Ctrl-C to stop.\n",
          args_.session_name.c_str(), args_.session_name.c_str());

#if PERFETTO_TP_UNIXD_HAS_SIGNALS()
  g_socket_path_for_signal = args_.socket_path.c_str();
  signal(SIGINT, HandleTermSignal);
  signal(SIGTERM, HandleTermSignal);
#endif

  task_runner_.Run();
  remove(args_.socket_path.c_str());
  return base::OkStatus();
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
}

}  // namespace

base::Status RunUnixRpcServer(Rpc& rpc, const UnixServerArgs& args) {
  UnixRpcServer server(rpc, args);
  return server.Run();
}

}  // namespace perfetto::trace_processor
