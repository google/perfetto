/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_HTTP_HTTP_SERVER_H_
#define INCLUDE_PERFETTO_EXT_BASE_HTTP_HTTP_SERVER_H_

#include <array>
#include <initializer_list>
#include <list>
#include <memory>
#include <string>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/unix_socket.h"

namespace perfetto {
namespace base {

class HttpServerConnection;

struct HttpRequest {
  explicit HttpRequest(HttpServerConnection* c) : conn(c) {}

  Optional<StringView> GetHeader(StringView name) const;

  HttpServerConnection* conn;

  // These StringViews point to memory in the rxbuf owned by |conn|. They are
  // valid only within the OnHttpRequest() call.
  StringView method;
  StringView uri;
  StringView origin;
  StringView body;

 private:
  friend class HttpServer;
  struct Header {
    StringView name;
    StringView value;
  };

  static constexpr uint32_t kMaxHeaders = 32;
  std::array<Header, kMaxHeaders> headers{};
  size_t num_headers = 0;
};

class HttpServerConnection {
 public:
  static constexpr size_t kOmitContentLength = static_cast<size_t>(-1);

  explicit HttpServerConnection(std::unique_ptr<UnixSocket>);
  ~HttpServerConnection();

  void SendResponseHeaders(const char* http_code,
                           std::initializer_list<const char*> headers = {},
                           size_t content_length = 0);

  // Works also for websockets.
  void SendResponseBody(const void* content, size_t content_length);
  void Close();

  // All the above in one shot.
  void SendResponse(const char* http_code,
                    std::initializer_list<const char*> headers = {},
                    StringView content = {},
                    bool force_close = false);
  void SendResponseAndClose(const char* http_code,
                            std::initializer_list<const char*> headers = {},
                            StringView content = {}) {
    SendResponse(http_code, headers, content, true);
  }

 private:
  friend class HttpServer;

  size_t rxbuf_avail() { return rxbuf.size() - rxbuf_used; }

  std::unique_ptr<UnixSocket> sock;
  PagedMemory rxbuf;
  size_t rxbuf_used = 0;
  bool headers_sent_ = false;
  size_t content_len_headers_ = 0;
  size_t content_len_actual_ = 0;

  // If the origin is in the server's |allowed_origins_| this contains the
  // origin itself. This is used to handle CORS headers.
  std::string origin_allowed_;

  // By default treat connections as keep-alive unless the client says
  // explicitly 'Connection: close'. This improves TraceProcessor's python API.
  // This is consistent with that nginx does.
  bool keepalive_ = true;
};

class HttpRequestHandler {
 public:
  virtual ~HttpRequestHandler();
  virtual void OnHttpRequest(const HttpRequest&) = 0;
  virtual void OnHttpConnectionClosed(HttpServerConnection*);
};

class HttpServer : public UnixSocket::EventListener {
 public:
  HttpServer(TaskRunner*, HttpRequestHandler*);
  ~HttpServer() override;
  void Start(int port);
  void AddAllowedOrigin(const std::string&);

 private:
  size_t ParseOneHttpRequest(HttpServerConnection*);
  void HandleCorsPreflightRequest(const HttpRequest&);
  bool IsOriginAllowed(StringView);

  // UnixSocket::EventListener implementation.
  void OnNewIncomingConnection(UnixSocket*,
                               std::unique_ptr<UnixSocket>) override;
  void OnConnect(UnixSocket* self, bool connected) override;
  void OnDisconnect(UnixSocket* self) override;
  void OnDataAvailable(UnixSocket* self) override;

  TaskRunner* const task_runner_;
  HttpRequestHandler* req_handler_;
  std::unique_ptr<UnixSocket> sock4_;
  std::unique_ptr<UnixSocket> sock6_;
  std::list<HttpServerConnection> clients_;
  std::list<std::string> allowed_origins_;
  bool origin_error_logged_ = false;
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_HTTP_HTTP_SERVER_H_
