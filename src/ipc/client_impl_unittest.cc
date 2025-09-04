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

#include "src/ipc/client_impl.h"

#include <stdio.h>

#include <string>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/ipc/service_descriptor.h"
#include "perfetto/ext/ipc/service_proxy.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "src/ipc/test/test_socket.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/ipc/wire_protocol.gen.h"
#include "src/ipc/test/client_unittest_messages.gen.h"

namespace perfetto {
namespace ipc {
namespace {

using ::perfetto::ipc::gen::ReplyProto;
using ::perfetto::ipc::gen::RequestProto;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Mock;

ipc::TestSocket kTestSocket{"client_impl_unittest"};

// A fake ServiceProxy. This fakes the client-side class that would be
// auto-generated from .proto-files.
class FakeProxy : public ServiceProxy {
 public:
  FakeProxy(const char* service_name, ServiceProxy::EventListener* el)
      : ServiceProxy(el), service_name_(service_name) {}

  const ServiceDescriptor& GetDescriptor() override {
    auto reply_decoder = [](const std::string& proto) {
      std::unique_ptr<ProtoMessage> reply(new ReplyProto());
      EXPECT_TRUE(reply->ParseFromString(proto));
      return reply;
    };
    if (!descriptor_.service_name) {
      descriptor_.service_name = service_name_;
      descriptor_.methods.push_back(
          {"FakeMethod1", nullptr, reply_decoder, nullptr});
    }
    return descriptor_;
  }

  const char* service_name_;
  ServiceDescriptor descriptor_;
};

class MockEventListener : public ServiceProxy::EventListener {
 public:
  MOCK_METHOD(void, OnConnect, (), (override));
  MOCK_METHOD(void, OnDisconnect, (), (override));
};

// A fake host implementation. Listens on |kTestSocket.name()| and replies to
// IPC metohds like a real one.
class FakeHost : public base::UnixSocket::EventListener {
 public:
  struct FakeMethod {
    MethodID id;
    MOCK_METHOD(void,
                OnInvoke,
                (const Frame::InvokeMethod&, Frame::InvokeMethodReply*));
  };  // FakeMethod.

  struct FakeService {
    FakeMethod* AddFakeMethod(const std::string& name) {
      auto it_and_inserted =
          methods.emplace(name, std::unique_ptr<FakeMethod>(new FakeMethod()));
      EXPECT_TRUE(it_and_inserted.second);
      FakeMethod* method = it_and_inserted.first->second.get();
      method->id = ++last_method_id;
      return method;
    }

    ServiceID id;
    std::map<std::string, std::unique_ptr<FakeMethod>> methods;
    MethodID last_method_id = 0;
  };  // FakeService.

  // If |should_listen| is set, then a listening socket is created.
  // Otherwise, incoming connections should be delivered via
  // OnNewIncomingConnection().
  FakeHost(bool should_listen, base::TaskRunner* task_runner) {
    if (should_listen) {
      kTestSocket.Destroy();
      listening_sock_ = base::UnixSocket::Listen(
          kTestSocket.name(), this, task_runner, kTestSocket.family(),
          base::SockType::kStream);
      EXPECT_TRUE(listening_sock_->is_listening());
    }
  }

  ~FakeHost() override { kTestSocket.Destroy(); }

  FakeService* AddFakeService(const std::string& name) {
    auto it_and_inserted = services_.emplace(
        name, std::unique_ptr<FakeService>(new FakeService()));
    EXPECT_TRUE(it_and_inserted.second);
    FakeService* svc = it_and_inserted.first->second.get();
    svc->id = ++last_service_id_;
    return svc;
  }

  // base::UnixSocket::EventListener implementation.
  void OnNewIncomingConnection(
      base::UnixSocket*,
      std::unique_ptr<base::UnixSocket> new_connection) override {
    ASSERT_FALSE(client_sock_);
    client_sock_ = std::move(new_connection);
  }

  void OnDataAvailable(base::UnixSocket* sock) override {
    if (sock != client_sock_.get())
      return;
    auto buf = frame_deserializer_.BeginReceive();
    base::ScopedFile fd;
    size_t rsize = client_sock_->Receive(buf.data, buf.size, &fd);
    if (fd)
      received_fd_ = std::move(fd);
    EXPECT_TRUE(frame_deserializer_.EndReceive(rsize));
    while (std::unique_ptr<Frame> frame = frame_deserializer_.PopNextFrame())
      OnFrameReceived(*frame);
  }

  void OnFrameReceived(const Frame& req) {
    if (req.has_msg_bind_service()) {
      auto svc_it = services_.find(req.msg_bind_service().service_name());
      ASSERT_NE(services_.end(), svc_it);
      const FakeService& svc = *svc_it->second;
      Frame reply;
      reply.set_request_id(req.request_id());
      reply.mutable_msg_bind_service_reply()->set_success(true);
      reply.mutable_msg_bind_service_reply()->set_service_id(svc.id);
      for (const auto& method_it : svc.methods) {
        auto* method = reply.mutable_msg_bind_service_reply()->add_methods();
        method->set_name(method_it.first);
        method->set_id(method_it.second->id);
      }
      Reply(reply);
    } else if (req.has_msg_invoke_method()) {
      // Lookup the service and method.
      bool has_more = false;
      do {
        Frame reply;
        reply.set_request_id(req.request_id());
        for (const auto& svc : services_) {
          if (svc.second->id != req.msg_invoke_method().service_id())
            continue;
          for (const auto& method : svc.second->methods) {
            if (method.second->id != req.msg_invoke_method().method_id())
              continue;
            method.second->OnInvoke(req.msg_invoke_method(),
                                    reply.mutable_msg_invoke_method_reply());
            has_more = reply.mutable_msg_invoke_method_reply()->has_more();
          }
        }
        // If either the method or the service are not found, |success| will be
        // false by default.
        Reply(reply);
      } while (has_more);
    } else {
      FAIL() << "Unknown request";
    }
  }

  void Reply(const Frame& frame) {
    if (suppress_replies_)
      return;
    auto buf = BufferedFrameDeserializer::Serialize(frame);
    ASSERT_TRUE(client_sock_->is_connected());
    EXPECT_TRUE(client_sock_->Send(buf.data(), buf.size(), next_reply_fd_));
    next_reply_fd_ = -1;
  }

  BufferedFrameDeserializer frame_deserializer_;
  std::unique_ptr<base::UnixSocket> listening_sock_;
  std::unique_ptr<base::UnixSocket> client_sock_;
  std::map<std::string, std::unique_ptr<FakeService>> services_;
  ServiceID last_service_id_ = 0;
  int next_reply_fd_ = -1;
  base::ScopedFile received_fd_;
  // This flag can be set to true to prevent the host from sending replies.
  // Useful to test race conditions in ClientImpl.
  bool suppress_replies_ = false;
};  // FakeHost.

class ClientImplTest : public ::testing::Test {
 public:
  void SetUp() override {
    task_runner_.reset(new base::TestTaskRunner());

#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
    auto socket_pair = base::UnixSocketRaw::CreatePairPosix(
        base::SockFamily::kUnix, base::SockType::kStream);
    host_.reset(new FakeHost(false, task_runner_.get()));
    host_->OnNewIncomingConnection(
        nullptr,
        base::UnixSocket::AdoptConnected(
            socket_pair.first.ReleaseFd(), host_.get(), task_runner_.get(),
            kTestSocket.family(), base::SockType::kStream));
    cli_ = Client::CreateInstance(Client::ConnArgs(base::ScopedSocketHandle(
                                      socket_pair.second.ReleaseFd())),
                                  task_runner_.get());
#else
    host_.reset(new FakeHost(true, task_runner_.get()));
    cli_ = Client::CreateInstance({kTestSocket.name(), /*retry=*/false},
                                  task_runner_.get());
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  }

  void TearDown() override {
    cli_.reset();
    host_.reset();
    task_runner_->RunUntilIdle();
    task_runner_.reset();
  }

  ::testing::StrictMock<MockEventListener> proxy_events_;
  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<FakeHost> host_;
  std::unique_ptr<Client> cli_;
};

TEST_F(ClientImplTest, BindAndInvokeMethod) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  auto* host_method = host_svc->AddFakeMethod("FakeMethod1");

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));

  // Bind |proxy| to the fake host.
  cli_->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  task_runner_->RunUntilCheckpoint("on_connect");

  // Invoke a valid method.
  EXPECT_CALL(*host_method, OnInvoke(_, _))
      .WillOnce(
          [](const Frame::InvokeMethod& req, Frame::InvokeMethodReply* reply) {
            RequestProto req_args;
            EXPECT_TRUE(req_args.ParseFromString(req.args_proto()));
            EXPECT_EQ("req_data", req_args.data());
            ReplyProto reply_args;
            reply->set_reply_proto(reply_args.SerializeAsString());
            reply->set_success(true);
          });

  RequestProto req;
  req.set_data("req_data");
  auto on_invoke_reply = task_runner_->CreateCheckpoint("on_invoke_reply");
  Deferred<ProtoMessage> deferred_reply(
      [on_invoke_reply](AsyncResult<ProtoMessage> reply) {
        EXPECT_TRUE(reply.success());
        on_invoke_reply();
      });
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply));
  task_runner_->RunUntilCheckpoint("on_invoke_reply");

  // Invoke an invalid method.
  auto on_invalid_invoke = task_runner_->CreateCheckpoint("on_invalid_invoke");
  Deferred<ProtoMessage> deferred_reply2(
      [on_invalid_invoke](AsyncResult<ProtoMessage> reply) {
        EXPECT_FALSE(reply.success());
        on_invalid_invoke();
      });
  RequestProto empty_req;
  proxy->BeginInvoke("InvalidMethod", empty_req, std::move(deferred_reply2));
  task_runner_->RunUntilCheckpoint("on_invalid_invoke");
}

// Tests that when invoking a method without binding a callback, the resulting
// request has the |drop_reply| flag set.
TEST_F(ClientImplTest, InvokeMethodDropReply) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  auto* host_method = host_svc->AddFakeMethod("FakeMethod1");

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));

  // Bind |proxy| to the fake host.
  cli_->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  task_runner_->RunUntilCheckpoint("on_connect");

  auto on_req_received = task_runner_->CreateCheckpoint("on_req_received");
  EXPECT_CALL(*host_method, OnInvoke(_, _))
      .WillOnce([on_req_received](const Frame::InvokeMethod& req,
                                  Frame::InvokeMethodReply*) {
        RequestProto req_args;
        EXPECT_TRUE(req.drop_reply());
        on_req_received();
      });

  // Invoke a method without binding any callback to the Deferred object.
  Deferred<ProtoMessage> no_callback;
  proxy->BeginInvoke("FakeMethod1", RequestProto(), std::move(no_callback));
  task_runner_->RunUntilCheckpoint("on_req_received");
}

// Like BindAndInvokeMethod, but this time invoke a streaming method that
// provides > 1 reply per invocation.
TEST_F(ClientImplTest, BindAndInvokeStreamingMethod) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  auto* host_method = host_svc->AddFakeMethod("FakeMethod1");
  const int kNumReplies = 3;

  // Create and bind |proxy| to the fake host.
  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));
  cli_->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  task_runner_->RunUntilCheckpoint("on_connect");

  // Invoke a valid method, reply kNumReplies times.
  int replies_left = kNumReplies;
  EXPECT_CALL(*host_method, OnInvoke(_, _))
      .Times(kNumReplies)
      .WillRepeatedly([&replies_left](const Frame::InvokeMethod& req,
                                      Frame::InvokeMethodReply* reply) {
        RequestProto req_args;
        EXPECT_TRUE(req_args.ParseFromString(req.args_proto()));
        reply->set_reply_proto(ReplyProto().SerializeAsString());
        reply->set_success(true);
        reply->set_has_more(--replies_left > 0);
      });

  RequestProto req;
  req.set_data("req_data");
  auto on_last_reply = task_runner_->CreateCheckpoint("on_last_reply");
  int replies_seen = 0;
  Deferred<ProtoMessage> deferred_reply(
      [on_last_reply, &replies_seen](AsyncResult<ProtoMessage> reply) {
        EXPECT_TRUE(reply.success());
        replies_seen++;
        if (!reply.has_more())
          on_last_reply();
      });
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply));
  task_runner_->RunUntilCheckpoint("on_last_reply");
  ASSERT_EQ(kNumReplies, replies_seen);
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
// File descriptor sending over IPC is not supported on Windows or Fuchsia.
TEST_F(ClientImplTest, ReceiveFileDescriptor) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  auto* host_method = host_svc->AddFakeMethod("FakeMethod1");

  // Create and bind |proxy| to the fake host.
  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));
  cli_->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  task_runner_->RunUntilCheckpoint("on_connect");

  base::TempFile tx_file = base::TempFile::CreateUnlinked();
  static constexpr char kFileContent[] = "shared file";
  ASSERT_EQ(static_cast<size_t>(base::WriteAll(tx_file.fd(), kFileContent,
                                               sizeof(kFileContent))),
            sizeof(kFileContent));
  host_->next_reply_fd_ = tx_file.fd();

  EXPECT_CALL(*host_method, OnInvoke(_, _))
      .WillOnce(
          [](const Frame::InvokeMethod&, Frame::InvokeMethodReply* reply) {
            RequestProto req_args;
            reply->set_reply_proto(ReplyProto().SerializeAsString());
            reply->set_success(true);
          });

  RequestProto req;
  auto on_reply = task_runner_->CreateCheckpoint("on_reply");
  Deferred<ProtoMessage> deferred_reply(
      [on_reply](AsyncResult<ProtoMessage> reply) {
        EXPECT_TRUE(reply.success());
        on_reply();
      });
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply));
  task_runner_->RunUntilCheckpoint("on_reply");

  tx_file.ReleaseFD();
  base::ScopedFile rx_fd = cli_->TakeReceivedFD();
  ASSERT_TRUE(rx_fd);
  char buf[sizeof(kFileContent)] = {};
  ASSERT_EQ(0, lseek(*rx_fd, 0, SEEK_SET));
  ASSERT_EQ(static_cast<long>(sizeof(buf)),
            PERFETTO_EINTR(read(*rx_fd, buf, sizeof(buf))));
  ASSERT_STREQ(kFileContent, buf);
}

TEST_F(ClientImplTest, SendFileDescriptor) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  auto* host_method = host_svc->AddFakeMethod("FakeMethod1");

  // Create and bind |proxy| to the fake host.
  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));
  cli_->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  task_runner_->RunUntilCheckpoint("on_connect");

  base::TempFile tx_file = base::TempFile::CreateUnlinked();
  static constexpr char kFileContent[] = "shared file";
  ASSERT_EQ(static_cast<size_t>(base::WriteAll(tx_file.fd(), kFileContent,
                                               sizeof(kFileContent))),
            sizeof(kFileContent));
  EXPECT_CALL(*host_method, OnInvoke(_, _))
      .WillOnce(
          [](const Frame::InvokeMethod&, Frame::InvokeMethodReply* reply) {
            RequestProto req_args;
            reply->set_reply_proto(ReplyProto().SerializeAsString());
            reply->set_success(true);
          });

  RequestProto req;
  auto on_reply = task_runner_->CreateCheckpoint("on_reply");
  Deferred<ProtoMessage> deferred_reply(
      [on_reply](AsyncResult<ProtoMessage> reply) {
        EXPECT_TRUE(reply.success());
        on_reply();
      });
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply),
                     tx_file.fd());
  task_runner_->RunUntilCheckpoint("on_reply");

  base::ScopedFile rx_fd = std::move(host_->received_fd_);
  ASSERT_TRUE(rx_fd);
  char buf[sizeof(kFileContent)] = {};
  ASSERT_EQ(0, lseek(*rx_fd, 0, SEEK_SET));
  ASSERT_EQ(static_cast<long>(sizeof(buf)),
            PERFETTO_EINTR(read(*rx_fd, buf, sizeof(buf))));
  ASSERT_STREQ(kFileContent, buf);
}
#endif  // !OS_WIN

TEST_F(ClientImplTest, BindSameServiceMultipleTimesShouldFail) {
  host_->AddFakeService("FakeSvc");

  std::unique_ptr<FakeProxy> proxy[3];
  for (size_t i = 0; i < base::ArraySize(proxy); i++)
    proxy[i].reset(new FakeProxy("FakeSvc", &proxy_events_));

  // Bind to the host.
  for (size_t i = 0; i < base::ArraySize(proxy); i++) {
    auto checkpoint_name = "on_connect_or_disconnect" + std::to_string(i);
    auto closure = task_runner_->CreateCheckpoint(checkpoint_name);
    if (i == 0) {
      // Only the first call should succeed.
      EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(closure);
    } else {
      EXPECT_CALL(proxy_events_, OnDisconnect()).WillOnce(closure);
    }
    cli_->BindService(proxy[i]->GetWeakPtr());
    task_runner_->RunUntilCheckpoint(checkpoint_name);
  }
}

TEST_F(ClientImplTest, BindRequestsAreQueuedIfNotConnected) {
  host_->AddFakeService("FakeSvc1");
  host_->AddFakeService("FakeSvc2");

  std::unique_ptr<FakeProxy> proxy1(new FakeProxy("FakeSvc1", &proxy_events_));
  std::unique_ptr<FakeProxy> proxy2(new FakeProxy("FakeSvc2", &proxy_events_));

  // Bind the services (in opposite order of creation) before running any task.
  cli_->BindService(proxy2->GetWeakPtr());
  cli_->BindService(proxy1->GetWeakPtr());

  InSequence seq;
  auto on_connect1 = task_runner_->CreateCheckpoint("on_connect1");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect1);

  auto on_connect2 = task_runner_->CreateCheckpoint("on_connect2");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect2);

  task_runner_->RunUntilCheckpoint("on_connect1");
  task_runner_->RunUntilCheckpoint("on_connect2");
}

// The deferred callbacks for both binding a service and invoking a method
// should be dropped if the ServiceProxy object is destroyed prematurely.
TEST_F(ClientImplTest, DropCallbacksIfServiceProxyIsDestroyed) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  auto* host_method = host_svc->AddFakeMethod("FakeMethod1");

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));

  // First bind the service but destroy it before ClientImpl manages to run any
  // tasks. No OnConnect() should be called.
  cli_->BindService(proxy->GetWeakPtr());
  proxy.reset();
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&proxy_events_));

  // Now bind it successfully, invoke a method but destroy the proxy before
  // the method reply is dispatched. The DeferredReply should be rejected,
  // despite the fact that the host gave a successful reply.
  proxy.reset(new FakeProxy("FakeSvc", &proxy_events_));
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  cli_->BindService(proxy->GetWeakPtr());
  task_runner_->RunUntilCheckpoint("on_connect");

  RequestProto req;
  auto on_reply_sent = task_runner_->CreateCheckpoint("on_reply_sent");
  EXPECT_CALL(*host_method, OnInvoke(_, _))
      .WillOnce([on_reply_sent](const Frame::InvokeMethod&,
                                Frame::InvokeMethodReply* reply) {
        ReplyProto reply_args;
        reply->set_success(true);
        on_reply_sent();
      });

  auto on_reject = task_runner_->CreateCheckpoint("on_reject");
  Deferred<ProtoMessage> deferred_reply(
      [on_reject](AsyncResult<ProtoMessage> res) {
        ASSERT_FALSE(res.success());
        on_reject();
      });
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply));
  proxy.reset();
  task_runner_->RunUntilCheckpoint("on_reject");
  task_runner_->RunUntilCheckpoint("on_reply_sent");
}

// If the Client object is destroyed before the ServiceProxy, the ServiceProxy
// should see a Disconnect() call and any pending callback should be rejected.
TEST_F(ClientImplTest, ClientDestroyedBeforeProxy) {
  auto* host_svc = host_->AddFakeService("FakeSvc");
  host_svc->AddFakeMethod("FakeMethod1");

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  cli_->BindService(proxy->GetWeakPtr());
  task_runner_->RunUntilCheckpoint("on_connect");

  auto on_reject = task_runner_->CreateCheckpoint("on_reject");
  DeferredBase deferred_reply([on_reject](AsyncResult<ProtoMessage> res) {
    ASSERT_FALSE(res.success());
    on_reject();
  });
  RequestProto req;
  proxy->BeginInvoke("FakeMethod1", req, std::move(deferred_reply));
  EXPECT_CALL(proxy_events_, OnDisconnect());
  cli_.reset();
  host_.reset();  // Prevent spurious OnInvoke callbacks on the fake host.
  task_runner_->RunUntilCheckpoint("on_reject");
}

// Test that OnDisconnect() is invoked if the host is not reachable.
TEST_F(ClientImplTest, HostNotReachable) {
  host_.reset();

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));

  auto on_disconnect = task_runner_->CreateCheckpoint("on_disconnect");
  EXPECT_CALL(proxy_events_, OnDisconnect()).WillOnce(on_disconnect);
  cli_->BindService(proxy->GetWeakPtr());
  task_runner_->RunUntilCheckpoint("on_disconnect");
}

// Test that OnDisconnect() is invoked if the host shuts down prematurely.
TEST_F(ClientImplTest, HostDisconnection) {
  host_->AddFakeService("FakeSvc");

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));

  // Bind |proxy| to the fake host.
  cli_->BindService(proxy->GetWeakPtr());
  auto on_connect = task_runner_->CreateCheckpoint("on_connect");
  EXPECT_CALL(proxy_events_, OnConnect()).WillOnce(on_connect);
  task_runner_->RunUntilCheckpoint("on_connect");

  auto on_disconnect = task_runner_->CreateCheckpoint("on_disconnect");
  EXPECT_CALL(proxy_events_, OnDisconnect()).WillOnce(on_disconnect);
  host_.reset();
  task_runner_->RunUntilCheckpoint("on_disconnect");
}

// Test that OnDisconnect() is invoked if the host shuts down prematurely.
TEST_F(ClientImplTest, HostDisconnectionBeforeBindReply) {
  host_->AddFakeService("FakeSvc");

  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));

  // Prevent the host from replying
  host_->suppress_replies_ = true;

  // Bind |proxy| to the fake host. The host will receive the request, but it
  // will not reply.
  cli_->BindService(proxy->GetWeakPtr());
  task_runner_->RunUntilIdle();
  auto on_disconnect = task_runner_->CreateCheckpoint("on_disconnect");
  EXPECT_CALL(proxy_events_, OnDisconnect()).WillOnce(on_disconnect);
  // Shutdown the host before it's able to send the Bind reply. The ClientImpl
  // should receive an OnDisconnect() callback and propagate that to the service
  // proxy.
  host_.reset();
  task_runner_->RunUntilCheckpoint("on_disconnect");
}

// Disabled on Fuchsia because Fuchsia kernel sockets are non-addressable
// so there is no connect() step which may fail.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
TEST_F(ClientImplTest, HostConnectionFailure) {
  ipc::TestSocket kNonexistentSock{"client_impl_unittest_nonexistent"};
  std::unique_ptr<Client> client = Client::CreateInstance(
      {kNonexistentSock.name(), /*retry=*/false}, task_runner_.get());

  // Connect a client to a non-existent socket, which will always fail. The
  // client will notify the proxy of disconnection.
  std::unique_ptr<FakeProxy> proxy(new FakeProxy("FakeSvc", &proxy_events_));
  client->BindService(proxy->GetWeakPtr());

  // Make sure the client copes with being deleted by the disconnection
  // callback.
  auto on_disconnect_reached = task_runner_->CreateCheckpoint("on_disconnect");
  auto on_disconnect = [&] {
    client.reset();
    on_disconnect_reached();
  };
  EXPECT_CALL(proxy_events_, OnDisconnect()).WillOnce(on_disconnect);
  task_runner_->RunUntilCheckpoint("on_disconnect");
}
#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)

// TODO(primiano): add the tests below.
// TEST(ClientImplTest, UnparsableReply) {}

}  // namespace
}  // namespace ipc
}  // namespace perfetto
