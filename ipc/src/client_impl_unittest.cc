/*
 * Copyright (C) 2017 The Android Open foo Project
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

#include "ipc/src/client_impl.h"

#include "base/test/test_task_runner.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ipc/service_descriptor.h"
#include "ipc/service_proxy.h"
#include "ipc/src/buffered_frame_deserializer.h"
#include "ipc/src/unix_socket.h"

#include "client_unittest_messages.pb.h"

namespace perfetto {
namespace ipc {
namespace {

using ::testing::_;
using ::testing::Invoke;

constexpr char kSockName[] = "/tmp/perfetto_client_impl_unittest.sock";

class FakeProxy : public ServiceProxy {
 public:
  static std::unique_ptr<ProtoMessage> ReplyDecoder(const std::string& proto) {
    std::unique_ptr<ProtoMessage> reply(new ReplyProto());
    EXPECT_TRUE(reply->ParseFromString(proto));
    return reply;
  }

  FakeProxy(const char* service_name, ServiceProxy::EventListener* el)
      : ServiceProxy(el), service_name_(service_name) {}

  const ServiceDescriptor& GetDescriptor() override {
    if (!descriptor_.service_name) {
      descriptor_.service_name = service_name_;
      descriptor_.methods.push_back({"FakeMethod1", nullptr, &ReplyDecoder});
    }
    return descriptor_;
  }

  const char* service_name_;
  ServiceDescriptor descriptor_;
};

class MockEventListener : public ServiceProxy::EventListener {
 public:
  MOCK_METHOD1(OnConnect, void(bool));
  MOCK_METHOD0(OnDisconnect, void());
};

class FakeHost : public UnixSocket::EventListener {
 public:
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD1(OnFrameReceived, std::unique_ptr<Frame>(const Frame&));

  explicit FakeHost(base::TaskRunner* task_runner) {
    unlink(kSockName);
    listening_sock = UnixSocket::Listen(kSockName, this, task_runner);
    EXPECT_TRUE(listening_sock->is_listening());
  }

  ~FakeHost() override { unlink(kSockName); }

  // UnixSocket::EventListener implementation.
  void OnNewIncomingConnection(
      UnixSocket*,
      std::unique_ptr<UnixSocket> new_connection) override {
    ASSERT_FALSE(client_sock);
    client_sock = std::move(new_connection);
  }

  void OnDisconnect(UnixSocket*) override { OnDisconnect(); }

  void OnDataAvailable(UnixSocket* sock) override {
    if (sock != client_sock.get())
      return;
    auto buf = frame_deserializer.BeginReceive();
    size_t rsize = client_sock->Receive(buf.data, buf.size);
    EXPECT_TRUE(frame_deserializer.EndReceive(rsize));
    while (std::unique_ptr<Frame> frame = frame_deserializer.PopNextFrame()) {
      std::unique_ptr<Frame> reply = OnFrameReceived(*frame);
      Reply(*reply);
    }
  }

  void Reply(const Frame& frame) {
    auto buf = BufferedFrameDeserializer::Serialize(frame);
    ASSERT_TRUE(client_sock->is_connected());
    EXPECT_TRUE(client_sock->Send(buf.data(), buf.size()));
  }

  BufferedFrameDeserializer frame_deserializer;
  std::unique_ptr<UnixSocket> listening_sock;
  std::unique_ptr<UnixSocket> client_sock;
};

TEST(ClientImplTest, BindAndInvokeMethod) {
  static constexpr ServiceID kServiceID = 42;
  static constexpr MethodID kMethodID = 13;

  base::TestTaskRunner task_runner;
  FakeHost host(&task_runner);
  std::unique_ptr<Client> cli = Client::CreateInstance(kSockName, &task_runner);
  MockEventListener event_listener;
  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &event_listener));

  // Bind to the host.
  EXPECT_CALL(host, OnFrameReceived(_)).WillOnce(Invoke([](const Frame& req) {
    EXPECT_EQ(Frame::kMsgBindService, req.msg_case());
    EXPECT_EQ("FakeSvc", req.msg_bind_service().service_name());
    std::unique_ptr<Frame> reply(new Frame());
    reply->set_request_id(req.request_id());
    reply->mutable_msg_bind_service_reply()->set_success(true);
    reply->mutable_msg_bind_service_reply()->set_service_id(kServiceID);
    auto* method = reply->mutable_msg_bind_service_reply()->add_methods();
    method->set_name("FakeMethod1");
    method->set_id(kMethodID);
    return reply;
  }));
  cli->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner.CreateCheckpoint("on_connect");
  EXPECT_CALL(event_listener, OnConnect(true))
      .WillOnce(Invoke([on_connect](bool) { on_connect(); }));
  task_runner.RunUntilCheckpoint("on_connect");

  // Invoke a valid method.
  EXPECT_CALL(host, OnFrameReceived(_)).WillOnce(Invoke([](const Frame& req) {
    EXPECT_EQ(Frame::kMsgInvokeMethod, req.msg_case());
    EXPECT_EQ(kServiceID, req.msg_invoke_method().service_id());
    EXPECT_EQ(kMethodID, req.msg_invoke_method().method_id());
    RequestProto req_args;
    EXPECT_TRUE(req_args.ParseFromString(req.msg_invoke_method().args_proto()));
    EXPECT_EQ("req_data", req_args.data());

    std::unique_ptr<Frame> reply(new Frame());
    reply->set_request_id(req.request_id());
    ReplyProto reply_args;
    reply->mutable_msg_invoke_method_reply()->set_reply_proto(
        reply_args.SerializeAsString());
    reply->mutable_msg_invoke_method_reply()->set_success(true);
    return reply;
  }));

  RequestProto req;
  req.set_data("req_data");
  auto on_invoke_reply = task_runner.CreateCheckpoint("on_invoke_reply");
  DeferredBase deferred_reply(
      [on_invoke_reply](AsyncResult<ProtoMessage> reply) {
        EXPECT_TRUE(reply.success());
        on_invoke_reply();
      });
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply));
  task_runner.RunUntilCheckpoint("on_invoke_reply");

  // Invoke an invalid method.
  EXPECT_CALL(host, OnFrameReceived(_)).WillOnce(Invoke([](const Frame& frame) {
    EXPECT_EQ(Frame::kMsgInvokeMethod, frame.msg_case());
    std::unique_ptr<Frame> reply(new Frame());
    reply->set_request_id(frame.request_id());
    reply->mutable_msg_invoke_method_reply()->set_success(false);
    return reply;
  }));

  auto on_invalid_invoke = task_runner.CreateCheckpoint("on_invalid_invoke");
  DeferredBase deferred_reply2(
      [on_invalid_invoke](AsyncResult<ProtoMessage> reply) {
        EXPECT_FALSE(reply.success());
        on_invalid_invoke();
      });
  RequestProto empty_req;
  proxy->BeginInvoke("FakeMethod1", empty_req, std::move(deferred_reply2));
  task_runner.RunUntilCheckpoint("on_invalid_invoke");
}

// TODO(primiano): add the tests below in next CLs.
// TEST(ClientImplTest, UnbindService) {}
// TEST(ClientImplTest, BindAndInvokeStreamingMethod) {}
// TEST(ClientImplTest, HostConnectionFailure) {}
// TEST(ClientImplTest, HostPrematureDisconnect) {}
// TEST(ClientImplTest, UnparsableReply) {}
// TEST(ClientImplTest, ProxyDestroyedBeforeClient) {}
// TEST(ClientImplTest, ClientDestroyedBeforeProxy) {}

}  // namespace
}  // namespace ipc
}  // namespace perfetto
