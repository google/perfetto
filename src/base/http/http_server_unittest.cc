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

#include <initializer_list>
#include <string>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/unix_socket.h"
#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using testing::_;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::NiceMock;

constexpr int kTestPort = 5127;  // Chosen with a fair dice roll.

class MockHttpHandler : public HttpRequestHandler {
 public:
  MOCK_METHOD1(OnHttpRequest, void(const HttpRequest&));
  MOCK_METHOD1(OnHttpConnectionClosed, void(HttpServerConnection*));
};

class HttpCli {
 public:
  explicit HttpCli(TestTaskRunner* ttr) : task_runner_(ttr) {
    sock = UnixSocketRaw::CreateMayFail(SockFamily::kInet, SockType::kStream);
    sock.SetBlocking(true);
    sock.Connect("127.0.0.1:" + std::to_string(kTestPort));
  }

  void SendHttpReq(std::initializer_list<std::string> headers,
                   const std::string& body = "") {
    for (auto& header : headers)
      sock.SendStr(header + "\r\n");
    if (!body.empty())
      sock.SendStr("Content-Length: " + std::to_string(body.size()) + "\r\n");
    sock.SendStr("\r\n");
    sock.SendStr(body);
  }

  std::string RecvAndWaitConnClose() {
    static int n = 0;
    auto checkpoint_name = "rx_" + std::to_string(n++);
    auto checkpoint = task_runner_->CreateCheckpoint(checkpoint_name);
    std::string rxbuf;
    sock.SetBlocking(false);
    task_runner_->AddFileDescriptorWatch(sock.fd(), [&] {
      char buf[1024]{};
      auto rsize = PERFETTO_EINTR(sock.Receive(buf, sizeof(buf)));
      ASSERT_GE(rsize, 0);
      if (rsize == 0)
        checkpoint();
      rxbuf.append(buf, static_cast<size_t>(rsize));
    });
    task_runner_->RunUntilCheckpoint(checkpoint_name);
    task_runner_->RemoveFileDescriptorWatch(sock.fd());
    return rxbuf;
  }

  TestTaskRunner* task_runner_;
  UnixSocketRaw sock;
};

class HttpServerTest : public ::testing::Test {
 public:
  HttpServerTest() : srv_(&task_runner_, &handler_) { srv_.Start(kTestPort); }

  TestTaskRunner task_runner_;
  MockHttpHandler handler_;
  HttpServer srv_;
};

TEST_F(HttpServerTest, GET) {
  const int kIterations = 3;
  EXPECT_CALL(handler_, OnHttpRequest(_))
      .Times(kIterations)
      .WillRepeatedly(Invoke([](const HttpRequest& req) {
        EXPECT_EQ(req.uri.ToStdString(), "/foo/bar");
        EXPECT_EQ(req.method.ToStdString(), "GET");
        EXPECT_EQ(req.origin.ToStdString(), "https://example.com");
        EXPECT_EQ("42",
                  req.GetHeader("X-header").value_or("N/A").ToStdString());
        EXPECT_EQ("foo",
                  req.GetHeader("X-header2").value_or("N/A").ToStdString());
        req.conn->SendResponseAndClose("200 OK", {}, "<html>");
      }));
  EXPECT_CALL(handler_, OnHttpConnectionClosed(_)).Times(kIterations);

  for (int i = 0; i < 3; i++) {
    HttpCli cli(&task_runner_);
    cli.SendHttpReq(
        {
            "GET /foo/bar HTTP/1.1",        //
            "Origin: https://example.com",  //
            "X-header: 42",                 //
            "X-header2: foo",               //
        },
        "");
    EXPECT_EQ(cli.RecvAndWaitConnClose(),
              "HTTP/1.1 200 OK\r\n"
              "Content-Length: 6\r\n"
              "Connection: close\r\n"
              "\r\n<html>");
  }
}

TEST_F(HttpServerTest, GET_404) {
  HttpCli cli(&task_runner_);
  EXPECT_CALL(handler_, OnHttpRequest(_))
      .WillOnce(Invoke([&](const HttpRequest& req) {
        EXPECT_EQ(req.uri.ToStdString(), "/404");
        EXPECT_EQ(req.method.ToStdString(), "GET");
        req.conn->SendResponseAndClose("404 Not Found");
      }));
  cli.SendHttpReq({"GET /404 HTTP/1.1"}, "");
  EXPECT_CALL(handler_, OnHttpConnectionClosed(_));
  EXPECT_EQ(cli.RecvAndWaitConnClose(),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n");
}

TEST_F(HttpServerTest, POST) {
  HttpCli cli(&task_runner_);

  EXPECT_CALL(handler_, OnHttpRequest(_))
      .WillOnce(Invoke([&](const HttpRequest& req) {
        EXPECT_EQ(req.uri.ToStdString(), "/rpc");
        EXPECT_EQ(req.method.ToStdString(), "POST");
        EXPECT_EQ(req.origin.ToStdString(), "https://example.com");
        EXPECT_EQ("foo", req.GetHeader("X-1").value_or("N/A").ToStdString());
        EXPECT_EQ(req.body.ToStdString(), "the\r\npost\nbody\r\n\r\n");
        req.conn->SendResponseAndClose("200 OK");
      }));

  cli.SendHttpReq(
      {"POST /rpc HTTP/1.1", "Origin: https://example.com", "X-1: foo"},
      "the\r\npost\nbody\r\n\r\n");
  EXPECT_CALL(handler_, OnHttpConnectionClosed(_));
  EXPECT_EQ(cli.RecvAndWaitConnClose(),
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n");
}

// An unhandled request should cause a HTTP 500.
TEST_F(HttpServerTest, Unhadled_500) {
  HttpCli cli(&task_runner_);
  EXPECT_CALL(handler_, OnHttpRequest(_));
  cli.SendHttpReq({"GET /unhandled HTTP/1.1"});
  EXPECT_CALL(handler_, OnHttpConnectionClosed(_));
  EXPECT_EQ(cli.RecvAndWaitConnClose(),
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n");
}

// Send three requests within the same keepalive connection.
TEST_F(HttpServerTest, POST_Keepalive) {
  HttpCli cli(&task_runner_);
  static const int kNumRequests = 3;
  int req_num = 0;
  EXPECT_CALL(handler_, OnHttpConnectionClosed(_)).Times(1);
  EXPECT_CALL(handler_, OnHttpRequest(_))
      .Times(3)
      .WillRepeatedly(Invoke([&](const HttpRequest& req) {
        EXPECT_EQ(req.uri.ToStdString(), "/" + std::to_string(req_num));
        EXPECT_EQ(req.method.ToStdString(), "POST");
        EXPECT_EQ(req.body.ToStdString(), "body" + std::to_string(req_num));
        req.conn->SendResponseHeaders("200 OK");
        if (++req_num == kNumRequests)
          req.conn->Close();
      }));

  for (int i = 0; i < kNumRequests; i++) {
    auto i_str = std::to_string(i);
    cli.SendHttpReq({"POST /" + i_str + " HTTP/1.1", "Connection: keep-alive"},
                    "body" + i_str);
  }

  std::string expected_response;
  for (int i = 0; i < kNumRequests; i++) {
    expected_response +=
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
  }
  EXPECT_EQ(cli.RecvAndWaitConnClose(), expected_response);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
