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

#include "src/ipc/host_impl.h"

#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/ipc/service.h"
#include "perfetto/ipc/service_descriptor.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "src/ipc/test/test_socket.h"
#include "src/ipc/unix_socket.h"

#include "src/ipc/test/client_unittest_messages.pb.h"
#include "src/ipc/wire_protocol.pb.h"

namespace perfetto {
namespace ipc {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;

constexpr char kSockName[] = TEST_SOCK_NAME("host_impl_unittest.sock");

// RequestProto and ReplyProto are defined in client_unittest_messages.proto.

class FakeService : public Service {
 public:
  MOCK_METHOD0(Destroyed, void());
  MOCK_METHOD2(OnFakeMethod1, void(const RequestProto&, DeferredBase*));

  static void Invoker(Service* service,
                      const ProtoMessage& req,
                      DeferredBase deferred_reply) {
    static_cast<FakeService*>(service)->OnFakeMethod1(
        static_cast<const RequestProto&>(req), &deferred_reply);
  }

  static std::unique_ptr<ProtoMessage> RequestDecoder(
      const std::string& proto) {
    std::unique_ptr<ProtoMessage> reply(new RequestProto());
    EXPECT_TRUE(reply->ParseFromString(proto));
    return reply;
  }

  FakeService(const char* service_name) {
    descriptor_.service_name = service_name;
    descriptor_.methods.push_back(
        {"FakeMethod1", &RequestDecoder, nullptr, &Invoker});
  }

  const ServiceDescriptor& GetDescriptor() override { return descriptor_; }

  ServiceDescriptor descriptor_;
};

class FakeClient : public UnixSocket::EventListener {
 public:
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD1(OnServiceBound, void(const Frame::BindServiceReply&));
  MOCK_METHOD1(OnInvokeMethodReply, void(const Frame::InvokeMethodReply&));
  MOCK_METHOD1(OnFileDescriptorReceived, void(int));
  MOCK_METHOD0(OnRequestError, void());

  explicit FakeClient(base::TaskRunner* task_runner) {
    sock_ = UnixSocket::Connect(kSockName, this, task_runner);
  }

  ~FakeClient() override = default;

  void BindService(const std::string& service_name) {
    Frame frame;
    uint64_t request_id = requests_.empty() ? 1 : requests_.rbegin()->first + 1;
    requests_.emplace(request_id, 0);
    frame.set_request_id(request_id);
    frame.mutable_msg_bind_service()->set_service_name(service_name);
    SendFrame(frame);
  }

  void InvokeMethod(ServiceID service_id,
                    MethodID method_id,
                    const ProtoMessage& args) {
    Frame frame;
    uint64_t request_id = requests_.empty() ? 1 : requests_.rbegin()->first + 1;
    requests_.emplace(request_id, 0);
    frame.set_request_id(request_id);
    frame.mutable_msg_invoke_method()->set_service_id(service_id);
    frame.mutable_msg_invoke_method()->set_method_id(method_id);
    frame.mutable_msg_invoke_method()->set_args_proto(args.SerializeAsString());
    SendFrame(frame);
  }

  // UnixSocket::EventListener implementation.
  void OnConnect(UnixSocket*, bool success) override {
    ASSERT_TRUE(success);
    OnConnect();
  }

  void OnDisconnect(UnixSocket*) override { OnDisconnect(); }

  void OnDataAvailable(UnixSocket* sock) override {
    ASSERT_EQ(sock_.get(), sock);
    auto buf = frame_deserializer_.BeginReceive();
    base::ScopedFile fd;
    size_t rsize = sock->Receive(buf.data, buf.size, &fd);
    ASSERT_TRUE(frame_deserializer_.EndReceive(rsize));
    if (fd)
      OnFileDescriptorReceived(*fd);
    while (std::unique_ptr<Frame> frame = frame_deserializer_.PopNextFrame()) {
      ASSERT_EQ(1u, requests_.count(frame->request_id()));
      EXPECT_EQ(0, requests_[frame->request_id()]++);
      if (frame->msg_case() == Frame::kMsgBindServiceReply) {
        if (frame->msg_bind_service_reply().success())
          last_bound_service_id_ = frame->msg_bind_service_reply().service_id();
        return OnServiceBound(frame->msg_bind_service_reply());
      }
      if (frame->msg_case() == Frame::kMsgInvokeMethodReply)
        return OnInvokeMethodReply(frame->msg_invoke_method_reply());
      if (frame->msg_case() == Frame::kMsgRequestError)
        return OnRequestError();
      FAIL() << "Unexpected frame received from host " << frame->msg_case();
    }
  }

  void SendFrame(const Frame& frame) {
    std::string buf = BufferedFrameDeserializer::Serialize(frame);
    ASSERT_TRUE(sock_->Send(buf.data(), buf.size()));
  }

  BufferedFrameDeserializer frame_deserializer_;
  std::unique_ptr<UnixSocket> sock_;
  std::map<uint64_t /* request_id */, int /* num_replies_received */> requests_;
  ServiceID last_bound_service_id_;
};

class HostImplTest : public ::testing::Test {
 public:
  void SetUp() override {
    DESTROY_TEST_SOCK(kSockName);
    task_runner_.reset(new base::TestTaskRunner());
    Host* host = Host::CreateInstance(kSockName, task_runner_.get()).release();
    ASSERT_NE(nullptr, host);
    host_.reset(static_cast<HostImpl*>(host));
    cli_.reset(new FakeClient(task_runner_.get()));
    auto on_connect = task_runner_->CreateCheckpoint("on_connect");
    EXPECT_CALL(*cli_, OnConnect()).WillOnce(Invoke(on_connect));
    task_runner_->RunUntilCheckpoint("on_connect");
  }

  void TearDown() override {
    task_runner_->RunUntilIdle();
    cli_.reset();
    host_.reset();
    task_runner_->RunUntilIdle();
    task_runner_.reset();
    DESTROY_TEST_SOCK(kSockName);
  }

  // ::testing::StrictMock<MockEventListener> proxy_events_;
  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<HostImpl> host_;
  std::unique_ptr<FakeClient> cli_;
};

TEST_F(HostImplTest, BindService) {
  // First bind the service when it doesn't exists yet and check that the
  // BindService() request fails.
  cli_->BindService("FakeService");  // FakeService does not exist yet.
  auto on_bind_failure = task_runner_->CreateCheckpoint("on_bind_failure");
  EXPECT_CALL(*cli_, OnServiceBound(_))
      .WillOnce(Invoke([on_bind_failure](const Frame::BindServiceReply& reply) {
        ASSERT_FALSE(reply.success());
        on_bind_failure();
      }));
  task_runner_->RunUntilCheckpoint("on_bind_failure");

  // Now expose the service and bind it.
  ASSERT_TRUE(host_->ExposeService(
      std::unique_ptr<Service>(new FakeService("FakeService"))));
  auto on_bind_success = task_runner_->CreateCheckpoint("on_bind_success");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_))
      .WillOnce(Invoke([on_bind_success](const Frame::BindServiceReply& reply) {
        ASSERT_TRUE(reply.success());
        on_bind_success();
      }));
  task_runner_->RunUntilCheckpoint("on_bind_success");
}

TEST_F(HostImplTest, InvokeNonExistingMethod) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  auto on_invoke_failure = task_runner_->CreateCheckpoint("on_invoke_failure");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 42, RequestProto());
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_))
      .WillOnce(
          Invoke([on_invoke_failure](const Frame::InvokeMethodReply& reply) {
            ASSERT_FALSE(reply.success());
            ASSERT_FALSE(reply.has_more());
            on_invoke_failure();
          }));
  task_runner_->RunUntilCheckpoint("on_invoke_failure");
}

TEST_F(HostImplTest, InvokeMethod) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  RequestProto req_args;
  req_args.set_data("foo");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args);
  auto on_reply_sent = task_runner_->CreateCheckpoint("on_reply_sent");
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce(
          Invoke([on_reply_sent](const RequestProto& req, DeferredBase* reply) {
            ASSERT_EQ("foo", req.data());
            std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
            reply_args->set_data("bar");
            reply->Resolve(AsyncResult<ProtoMessage>(
                std::unique_ptr<ProtoMessage>(reply_args.release())));
            on_reply_sent();
          }));
  task_runner_->RunUntilCheckpoint("on_reply_sent");

  auto on_reply_received = task_runner_->CreateCheckpoint("on_reply_received");
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_))
      .WillOnce(
          Invoke([on_reply_received](const Frame::InvokeMethodReply& reply) {
            ASSERT_TRUE(reply.success());
            ASSERT_FALSE(reply.has_more());
            ReplyProto reply_args;
            reply_args.ParseFromString(reply.reply_proto());
            ASSERT_EQ("bar", reply_args.data());
            on_reply_received();
          }));
  task_runner_->RunUntilCheckpoint("on_reply_received");
}

TEST_F(HostImplTest, SendFileDescriptor) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  static constexpr char kFileContent[] = "shared file";
  RequestProto req_args;
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args);
  auto on_reply_sent = task_runner_->CreateCheckpoint("on_reply_sent");
  FILE* tx_file = tmpfile();
  fwrite(kFileContent, sizeof(kFileContent), 1, tx_file);
  fflush(tx_file);
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce(Invoke([on_reply_sent, tx_file](const RequestProto& req,
                                                DeferredBase* reply) {
        std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
        auto async_res = AsyncResult<ProtoMessage>(
            std::unique_ptr<ProtoMessage>(reply_args.release()));
        async_res.set_fd(fileno(tx_file));
        reply->Resolve(std::move(async_res));
        on_reply_sent();
      }));
  task_runner_->RunUntilCheckpoint("on_reply_sent");
  fclose(tx_file);

  auto on_fd_received = task_runner_->CreateCheckpoint("on_fd_received");
  EXPECT_CALL(*cli_, OnFileDescriptorReceived(_))
      .WillOnce(Invoke([on_fd_received](int fd) {
        char buf[sizeof(kFileContent)] = {};
        ASSERT_EQ(0, lseek(fd, 0, SEEK_SET));
        ASSERT_EQ(static_cast<int32_t>(sizeof(buf)),
                  PERFETTO_EINTR(read(fd, buf, sizeof(buf))));
        ASSERT_STREQ(kFileContent, buf);
        on_fd_received();
      }));
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_));
  task_runner_->RunUntilCheckpoint("on_fd_received");
}

// Invoke a method and immediately after disconnect the client.
TEST_F(HostImplTest, OnClientDisconnect) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  RequestProto req_args;
  req_args.set_data("foo");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args);
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_)).Times(0);
  cli_.reset();  // Disconnect the client.
  auto on_host_method = task_runner_->CreateCheckpoint("on_host_method");
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce(Invoke(
          [on_host_method](const RequestProto& req, DeferredBase* reply) {
            ASSERT_EQ("foo", req.data());
            on_host_method();
          }));
  task_runner_->RunUntilCheckpoint("on_host_method");
}

// Like InvokeMethod, but instead of resolving the Deferred reply within the
// call stack, std::move()-s it outside an replies
TEST_F(HostImplTest, MoveReplyObjectAndReplyAsynchronously) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  // Invokes the remote method and waits that the FakeService sees it. The reply
  // is not resolved but just moved into |moved_reply|.
  RequestProto req_args;
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args);
  auto on_invoke = task_runner_->CreateCheckpoint("on_invoke");
  DeferredBase moved_reply;
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce(Invoke([on_invoke, &moved_reply](const RequestProto& req,
                                                 DeferredBase* reply) {
        moved_reply = std::move(*reply);
        on_invoke();
      }));
  task_runner_->RunUntilCheckpoint("on_invoke");

  // Check that the FakeClient doesn't see any reply yet.
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_)).Times(0);
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(cli_.get()));

  // Resolve the reply asynchronously in a deferred task.
  task_runner_->PostTask([&moved_reply] {
    std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
    reply_args->set_data("bar");
    moved_reply.Resolve(AsyncResult<ProtoMessage>(
        std::unique_ptr<ProtoMessage>(reply_args.release())));
  });

  auto on_reply_received = task_runner_->CreateCheckpoint("on_reply_received");
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_))
      .WillOnce(
          Invoke([on_reply_received](const Frame::InvokeMethodReply& reply) {
            ASSERT_TRUE(reply.success());
            ASSERT_FALSE(reply.has_more());
            ReplyProto reply_args;
            reply_args.ParseFromString(reply.reply_proto());
            ASSERT_EQ("bar", reply_args.data());
            on_reply_received();
          }));
  task_runner_->RunUntilCheckpoint("on_reply_received");
}

// TODO(primiano): add the tests below in next CLs.
// TEST(HostImplTest, ManyClients) {}
// TEST(HostImplTest, OverlappingRequstsOutOfOrder) {}
// TEST(HostImplTest, StreamingRequest) {}

}  // namespace
}  // namespace ipc
}  // namespace perfetto
