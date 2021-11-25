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
#include "perfetto/ext/base/http/http_server.h"

#include <vector>

#include "perfetto/ext/base/endian.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto {
namespace base {

namespace {
// 32 MiB payload + 128K for HTTP headers.
constexpr size_t kMaxRequestSize = (32 * 1024 + 128) * 1024;

}  // namespace

HttpServer::HttpServer(TaskRunner* task_runner, HttpRequestHandler* req_handler)
    : task_runner_(task_runner), req_handler_(req_handler) {}
HttpServer::~HttpServer() = default;

void HttpServer::Start(int port) {
  std::string ipv4_addr = "127.0.0.1:" + std::to_string(port);
  std::string ipv6_addr = "[::1]:" + std::to_string(port);

  sock4_ = UnixSocket::Listen(ipv4_addr, this, task_runner_, SockFamily::kInet,
                              SockType::kStream);
  bool ipv4_listening = sock4_ && sock4_->is_listening();
  if (!ipv4_listening) {
    PERFETTO_PLOG("Failed to listen on IPv4 socket");
    sock4_.reset();
  }

  sock6_ = UnixSocket::Listen(ipv6_addr, this, task_runner_, SockFamily::kInet6,
                              SockType::kStream);
  bool ipv6_listening = sock6_ && sock6_->is_listening();
  if (!ipv6_listening) {
    PERFETTO_PLOG("Failed to listen on IPv6 socket");
    sock6_.reset();
  }
}

void HttpServer::AddAllowedOrigin(const std::string& origin) {
  allowed_origins_.emplace_back(origin);
}

void HttpServer::OnNewIncomingConnection(
    UnixSocket*,  // The listening socket, irrelevant here.
    std::unique_ptr<UnixSocket> sock) {
  PERFETTO_LOG("[HTTP] New connection");
  clients_.emplace_back(std::move(sock));
}

void HttpServer::OnConnect(UnixSocket*, bool) {}

void HttpServer::OnDisconnect(UnixSocket* sock) {
  PERFETTO_LOG("[HTTP] Client disconnected");
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (it->sock.get() == sock) {
      req_handler_->OnHttpConnectionClosed(&*it);
      clients_.erase(it);
      return;
    }
  }
  PERFETTO_DFATAL("[HTTP] Untracked client in OnDisconnect()");
}

void HttpServer::OnDataAvailable(UnixSocket* sock) {
  HttpServerConnection* conn = nullptr;
  for (auto it = clients_.begin(); it != clients_.end() && !conn; ++it)
    conn = (it->sock.get() == sock) ? &*it : nullptr;
  PERFETTO_CHECK(conn);

  char* rxbuf = reinterpret_cast<char*>(conn->rxbuf.Get());
  for (;;) {
    size_t avail = conn->rxbuf_avail();
    PERFETTO_CHECK(avail <= kMaxRequestSize);
    if (avail == 0) {
      conn->SendResponseAndClose("413 Payload Too Large");
      return;
    }
    size_t rsize = sock->Receive(&rxbuf[conn->rxbuf_used], avail);
    conn->rxbuf_used += rsize;
    if (rsize == 0 || conn->rxbuf_avail() == 0)
      break;
  }

  // At this point |rxbuf| can contain a partial HTTP request, a full one or
  // more (in case of HTTP Keepalive pipelining).
  for (;;) {
    size_t bytes_consumed = ParseOneHttpRequest(conn);

    if (bytes_consumed == 0)
      break;
    memmove(rxbuf, &rxbuf[bytes_consumed], conn->rxbuf_used - bytes_consumed);
    conn->rxbuf_used -= bytes_consumed;
  }
}

// Parses the HTTP request and invokes HandleRequest(). It returns the size of
// the HTTP header + body that has been processed or 0 if there isn't enough
// data for a full HTTP request in the buffer.
size_t HttpServer::ParseOneHttpRequest(HttpServerConnection* conn) {
  auto* rxbuf = reinterpret_cast<char*>(conn->rxbuf.Get());
  StringView buf_view(rxbuf, conn->rxbuf_used);
  bool has_parsed_first_line = false;
  bool all_headers_received = false;
  HttpRequest http_req(conn);
  size_t body_size = 0;

  // This loop parses the HTTP request headers and sets the |body_offset|.
  while (!buf_view.empty()) {
    size_t next = buf_view.find('\n');
    if (next == StringView::npos)
      break;
    StringView line = buf_view.substr(0, next);
    buf_view = buf_view.substr(next + 1);  // Eat the current line.
    while (!line.empty() && (line.at(line.size() - 1) == '\r' ||
                             line.at(line.size() - 1) == '\n')) {
      line = line.substr(0, line.size() - 1);
    }

    if (!has_parsed_first_line) {
      // Parse the "GET /xxx HTTP/1.1" line.
      has_parsed_first_line = true;
      size_t space = line.find(' ');
      if (space == std::string::npos || space + 2 >= line.size()) {
        conn->SendResponseAndClose("400 Bad Request");
        return 0;
      }
      http_req.method = line.substr(0, space);
      size_t uri_size = line.find(' ', space + 1) - (space + 1);
      http_req.uri = line.substr(space + 1, uri_size);
    } else if (line.empty()) {
      all_headers_received = true;
      // The CR-LF marker that separates headers from body.
      break;
    } else {
      // Parse HTTP headers, e.g. "Content-Length: 1234".
      size_t col = line.find(':');
      if (col == StringView::npos) {
        PERFETTO_DLOG("[HTTP] Malformed HTTP header: \"%s\"",
                      line.ToStdString().c_str());
        conn->SendResponseAndClose("400 Bad Request", {}, "Bad HTTP header");
        return 0;
      }
      auto hdr_name = line.substr(0, col);
      auto hdr_value = line.substr(col + 2);
      if (http_req.num_headers < http_req.headers.size()) {
        http_req.headers[http_req.num_headers++] = {hdr_name, hdr_value};
      } else {
        conn->SendResponseAndClose("400 Bad Request", {},
                                   "Too many HTTP headers");
      }

      if (hdr_name.CaseInsensitiveEq("content-length")) {
        body_size = static_cast<size_t>(atoi(hdr_value.ToStdString().c_str()));
      } else if (hdr_name.CaseInsensitiveEq("origin")) {
        http_req.origin = hdr_value;
        if (IsOriginAllowed(hdr_value))
          conn->origin_allowed_ = hdr_value.ToStdString();
      } else if (hdr_name.CaseInsensitiveEq("connection")) {
        conn->keepalive_ = hdr_value.CaseInsensitiveEq("keep-alive");
      }
    }
  }

  // At this point |buf_view| has been stripped of the header and contains the
  // request body. We don't know yet if we have all the bytes for it or not.
  PERFETTO_CHECK(buf_view.size() <= conn->rxbuf_used);
  const size_t headers_size = conn->rxbuf_used - buf_view.size();

  if (body_size + headers_size >= kMaxRequestSize) {
    conn->SendResponseAndClose("413 Payload Too Large");
    return 0;
  }

  // If we can't read the full request return and try again next time with more
  // data.
  if (!all_headers_received || buf_view.size() < body_size)
    return 0;

  http_req.body = buf_view.substr(0, body_size);

  PERFETTO_LOG("[HTTP] %.*s %.*s [body=%zuB, origin=\"%.*s\"]",
               static_cast<int>(http_req.method.size()), http_req.method.data(),
               static_cast<int>(http_req.uri.size()), http_req.uri.data(),
               http_req.body.size(), static_cast<int>(http_req.origin.size()),
               http_req.origin.data());

  if (http_req.method == "OPTIONS") {
    HandleCorsPreflightRequest(http_req);
  } else {
    // Let the HttpHandler handle the request.
    req_handler_->OnHttpRequest(http_req);
  }

  // The handler is expected to send a response. If not, bail with a HTTP 500.
  if (!conn->headers_sent_)
    conn->SendResponseAndClose("500 Internal Server Error");

  // Allow chaining multiple responses in the same HTTP-Keepalive connection.
  conn->headers_sent_ = false;

  return headers_size + body_size;
}

void HttpServer::HandleCorsPreflightRequest(const HttpRequest& req) {
  req.conn->SendResponseAndClose(
      "204 No Content",
      {
          "Access-Control-Allow-Methods: POST, GET, OPTIONS",  //
          "Access-Control-Allow-Headers: *",                   //
          "Access-Control-Max-Age: 86400",                     //
      });
}

bool HttpServer::IsOriginAllowed(StringView origin) {
  for (const std::string& allowed_origin : allowed_origins_) {
    if (origin.CaseInsensitiveEq(StringView(allowed_origin))) {
      return true;
    }
  }
  if (!origin_error_logged_ && !origin.empty()) {
    origin_error_logged_ = true;
    PERFETTO_ELOG(
        "[HTTP] The origin \"%.*s\" is not allowed, Access-Control-Allow-Origin"
        " won't be emitted. If this request comes from a browser it will fail.",
        static_cast<int>(origin.size()), origin.data());
  }
  return false;
}

void HttpServerConnection::SendResponseHeaders(
    const char* http_code,
    std::initializer_list<const char*> headers,
    size_t content_length) {
  PERFETTO_CHECK(!headers_sent_);
  headers_sent_ = true;
  std::vector<char> resp_hdr;
  resp_hdr.reserve(512);
  bool has_connection_header = false;

  auto append = [&resp_hdr](const char* str) {
    resp_hdr.insert(resp_hdr.end(), str, str + strlen(str));
  };

  append("HTTP/1.1 ");
  append(http_code);
  append("\r\n");
  for (const char* hdr : headers) {
    if (strlen(hdr) == 0)
      continue;
    has_connection_header |= strncasecmp(hdr, "connection:", 11) == 0;
    append(hdr);
    append("\r\n");
  }
  content_len_actual_ = 0;
  content_len_headers_ = content_length;
  if (content_length != kOmitContentLength) {
    append("Content-Length: ");
    append(std::to_string(content_length).c_str());
    append("\r\n");
  }
  if (!has_connection_header) {
    // Various clients (e.g., python's http.client) assume that a HTTP
    // connection is keep-alive if the server says nothing, even when they do
    // NOT ask for it. Hence we must be explicit. If we are about to close the
    // connection, we must say so.
    append(keepalive_ ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
  }
  if (!origin_allowed_.empty()) {
    append("Access-Control-Allow-Origin: ");
    append(origin_allowed_.c_str());
    append("\r\n");
    append("Vary: Origin\r\n");
  }
  append("\r\n");  // End-of-headers marker.
  sock->Send(resp_hdr.data(),
             resp_hdr.size());  // Send response headers.
}

void HttpServerConnection::SendResponseBody(const void* data, size_t len) {
  if (data == nullptr) {
    PERFETTO_DCHECK(len == 0);
    return;
  }
  content_len_actual_ += len;
  PERFETTO_CHECK(content_len_actual_ <= content_len_headers_ ||
                 content_len_headers_ == kOmitContentLength);
  sock->Send(data, len);
}

void HttpServerConnection::Close() {
  sock->Shutdown(/*notify=*/true);
}

void HttpServerConnection::SendResponse(
    const char* http_code,
    std::initializer_list<const char*> headers,
    StringView content,
    bool force_close) {
  if (force_close)
    keepalive_ = false;
  SendResponseHeaders(http_code, headers, content.size());
  SendResponseBody(content.data(), content.size());
  if (!keepalive_)
    Close();
}

HttpServerConnection::HttpServerConnection(std::unique_ptr<UnixSocket> s)
    : sock(std::move(s)), rxbuf(PagedMemory::Allocate(kMaxRequestSize)) {}

HttpServerConnection::~HttpServerConnection() = default;

Optional<StringView> HttpRequest::GetHeader(StringView name) const {
  for (size_t i = 0; i < num_headers; i++) {
    if (headers[i].name.CaseInsensitiveEq(name))
      return headers[i].value;
  }
  return nullopt;
}

HttpRequestHandler::~HttpRequestHandler() = default;
void HttpRequestHandler::OnHttpConnectionClosed(HttpServerConnection*) {}

}  // namespace base
}  // namespace perfetto
