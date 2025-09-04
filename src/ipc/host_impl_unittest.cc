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

#include "src/ipc/host_impl.h"

#include <memory>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/sys_types.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/ipc/service.h"
#include "perfetto/ext/ipc/service_descriptor.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "src/ipc/test/test_socket.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/ipc/wire_protocol.gen.h"
#include "src/ipc/test/client_unittest_messages.gen.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/socket.h>
#endif

namespace perfetto {
namespace ipc {
namespace {

using ::perfetto::ipc::Frame;
using ::perfetto::ipc::gen::ReplyProto;
using ::perfetto::ipc::gen::RequestProto;
using ::testing::_;
using ::testing::InvokeWithoutArgs;
using ::testing::Return;

ipc::TestSocket kTestSocket{"host_impl_unittest"};

// RequestProto and ReplyProto are defined in client_unittest_messages.proto.

class FakeService : public Service {
 public:
  MOCK_METHOD(void, OnFakeMethod1, (const RequestProto&, DeferredBase*));

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

  explicit FakeService(const char* service_name) {
    descriptor_.service_name = service_name;
    descriptor_.methods.push_back(
        {"FakeMethod1", &RequestDecoder, nullptr, &Invoker});
  }

  const ServiceDescriptor& GetDescriptor() override { return descriptor_; }

  base::ScopedFile TakeReceivedFD() { return ipc::Service::TakeReceivedFD(); }

  base::ScopedFile received_fd_;
  ServiceDescriptor descriptor_;
};

class FakeClient : public base::UnixSocket::EventListener {
 public:
  MOCK_METHOD(void, OnConnect, ());
  MOCK_METHOD(void, OnDisconnect, ());
  MOCK_METHOD(void, OnServiceBound, (const Frame::BindServiceReply&));
  MOCK_METHOD(void, OnInvokeMethodReply, (const Frame::InvokeMethodReply&));
  MOCK_METHOD(void, OnFileDescriptorReceived, (int));
  MOCK_METHOD(void, OnRequestError, ());

  explicit FakeClient(base::TaskRunner* task_runner) {
    sock_ = base::UnixSocket::Connect(kTestSocket.name(), this, task_runner,
                                      kTestSocket.family(),
                                      base::SockType::kStream);
  }

  FakeClient(const char* sock_name, base::TaskRunner* task_runner) {
    auto sock_family = base::GetSockFamily(sock_name);
    sock_ = base::UnixSocket::Connect(sock_name, this, task_runner, sock_family,
                                      base::SockType::kStream);
  }

  FakeClient(base::ScopedSocketHandle connected_socket,
             base::TaskRunner* task_runner) {
    sock_ = base::UnixSocket::AdoptConnected(std::move(connected_socket), this,
                                             task_runner, kTestSocket.family(),
                                             base::SockType::kStream);
    task_runner->PostTask([this]() { OnConnect(); });
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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  void SetPeerIdentity(uid_t uid,
                       pid_t pid,
                       const std::string& machine_id_hint) {
    Frame ipc_frame;
    ipc_frame.set_request_id(0);
    auto* set_peer_identity = ipc_frame.mutable_set_peer_identity();
    set_peer_identity->set_pid(pid);
    set_peer_identity->set_uid(static_cast<int32_t>(uid));
    set_peer_identity->set_machine_id_hint(machine_id_hint);
    SendFrame(ipc_frame);
  }
#endif

  void InvokeMethod(ServiceID service_id,
                    MethodID method_id,
                    const ProtoMessage& args,
                    bool drop_reply = false,
                    int fd = -1) {
    Frame frame;
    uint64_t request_id = requests_.empty() ? 1 : requests_.rbegin()->first + 1;
    requests_.emplace(request_id, 0);
    frame.set_request_id(request_id);
    frame.mutable_msg_invoke_method()->set_service_id(service_id);
    frame.mutable_msg_invoke_method()->set_method_id(method_id);
    frame.mutable_msg_invoke_method()->set_drop_reply(drop_reply);
    frame.mutable_msg_invoke_method()->set_args_proto(args.SerializeAsString());
    SendFrame(frame, fd);
  }

  // base::UnixSocket::EventListener implementation.
  void OnConnect(base::UnixSocket*, bool success) override {
    ASSERT_TRUE(success);
    OnConnect();
  }

  void OnDisconnect(base::UnixSocket*) override { OnDisconnect(); }

  void OnDataAvailable(base::UnixSocket* sock) override {
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
      if (frame->has_msg_bind_service_reply()) {
        if (frame->msg_bind_service_reply().success())
          last_bound_service_id_ = frame->msg_bind_service_reply().service_id();
        return OnServiceBound(frame->msg_bind_service_reply());
      }
      if (frame->has_msg_invoke_method_reply())
        return OnInvokeMethodReply(frame->msg_invoke_method_reply());
      if (frame->has_msg_request_error())
        return OnRequestError();
      FAIL() << "Unexpected frame received from host";
    }
  }

  void SendFrame(const Frame& frame, int fd = -1) {
    std::string buf = BufferedFrameDeserializer::Serialize(frame);
    ASSERT_TRUE(sock_->Send(buf.data(), buf.size(), fd));
  }

  BufferedFrameDeserializer frame_deserializer_;
  std::unique_ptr<base::UnixSocket> sock_;
  std::map<uint64_t /* request_id */, int /* num_replies_received */> requests_;
  ServiceID last_bound_service_id_;
};

class HostImplTest : public ::testing::Test {
 public:
  void SetUp() override {
    kTestSocket.Destroy();
    task_runner_.reset(new base::TestTaskRunner());
#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
    Host* host = Host::CreateInstance_Fuchsia(task_runner_.get()).release();
    auto socket_pair = base::UnixSocketRaw::CreatePairPosix(
        base::SockFamily::kUnix, base::SockType::kStream);
    host->AdoptConnectedSocket_Fuchsia(
        base::ScopedSocketHandle(socket_pair.first.ReleaseFd()),
        [](int) { return false; });
    cli_.reset(
        new FakeClient(base::ScopedSocketHandle(socket_pair.second.ReleaseFd()),
                       task_runner_.get()));
#else
    Host* host =
        Host::CreateInstance(kTestSocket.name(), task_runner_.get()).release();
    cli_.reset(new FakeClient(task_runner_.get()));
#endif
    ASSERT_NE(nullptr, host);
    host_.reset(static_cast<HostImpl*>(host));
    auto on_connect = task_runner_->CreateCheckpoint("on_connect");
    EXPECT_CALL(*cli_, OnConnect()).WillOnce(on_connect);
    task_runner_->RunUntilCheckpoint("on_connect");
  }

  void TearDown() override {
    task_runner_->RunUntilIdle();
    cli_.reset();
    host_.reset();
    task_runner_->RunUntilIdle();
    task_runner_.reset();
    kTestSocket.Destroy();
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
      .WillOnce([on_bind_failure](const Frame::BindServiceReply& reply) {
        ASSERT_FALSE(reply.success());
        on_bind_failure();
      });
  task_runner_->RunUntilCheckpoint("on_bind_failure");

  // Now expose the service and bind it.
  ASSERT_TRUE(host_->ExposeService(
      std::unique_ptr<Service>(new FakeService("FakeService"))));
  auto on_bind_success = task_runner_->CreateCheckpoint("on_bind_success");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_))
      .WillOnce([on_bind_success](const Frame::BindServiceReply& reply) {
        ASSERT_TRUE(reply.success());
        on_bind_success();
      });
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
      .WillOnce([on_invoke_failure](const Frame::InvokeMethodReply& reply) {
        ASSERT_FALSE(reply.success());
        ASSERT_FALSE(reply.has_more());
        on_invoke_failure();
      });
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
      .WillOnce([on_reply_sent](const RequestProto& req, DeferredBase* reply) {
        ASSERT_EQ("foo", req.data());
        std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
        reply_args->set_data("bar");
        reply->Resolve(AsyncResult<ProtoMessage>(
            std::unique_ptr<ProtoMessage>(reply_args.release())));
        on_reply_sent();
      });
  task_runner_->RunUntilCheckpoint("on_reply_sent");

  auto on_reply_received = task_runner_->CreateCheckpoint("on_reply_received");
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_))
      .WillOnce([on_reply_received](const Frame::InvokeMethodReply& reply) {
        ASSERT_TRUE(reply.success());
        ASSERT_FALSE(reply.has_more());
        ReplyProto reply_args;
        reply_args.ParseFromString(reply.reply_proto());
        ASSERT_EQ("bar", reply_args.data());
        on_reply_received();
      });
  task_runner_->RunUntilCheckpoint("on_reply_received");
}

TEST_F(HostImplTest, InvokeMethodDropReply) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  // OnFakeMethod1 will:
  // - Do nothing on the 1st call, when |drop_reply| == true.
  // - Reply on the 2nd call, when |drop_reply| == false.
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .Times(2)
      .WillRepeatedly([](const RequestProto& req, DeferredBase* reply) {
        if (req.data() == "drop_reply")
          return;
        std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
        reply_args->set_data("the_reply");
        reply->Resolve(AsyncResult<ProtoMessage>(
            std::unique_ptr<ProtoMessage>(reply_args.release())));
      });

  auto on_reply_received = task_runner_->CreateCheckpoint("on_reply_received");
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_))
      .WillOnce([on_reply_received](const Frame::InvokeMethodReply& reply) {
        ASSERT_TRUE(reply.success());
        ReplyProto reply_args;
        reply_args.ParseFromString(reply.reply_proto());
        ASSERT_EQ("the_reply", reply_args.data());
        on_reply_received();
      });

  // Invoke the method first with |drop_reply|=true, then |drop_reply|=false.
  RequestProto rp;
  rp.set_data("drop_reply");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, rp, true /*drop_reply*/);
  rp.set_data("do_reply");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, rp, false /*drop_reply*/);

  task_runner_->RunUntilCheckpoint("on_reply_received");
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
// File descriptor sending over IPC is not supported on Windows.
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
  base::TempFile tx_file = base::TempFile::CreateUnlinked();
  ASSERT_EQ(static_cast<size_t>(base::WriteAll(tx_file.fd(), kFileContent,
                                               sizeof(kFileContent))),
            sizeof(kFileContent));
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce(
          [on_reply_sent, &tx_file](const RequestProto&, DeferredBase* reply) {
            std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
            auto async_res = AsyncResult<ProtoMessage>(
                std::unique_ptr<ProtoMessage>(reply_args.release()));
            async_res.set_fd(tx_file.fd());
            reply->Resolve(std::move(async_res));
            on_reply_sent();
          });
  task_runner_->RunUntilCheckpoint("on_reply_sent");
  tx_file.ReleaseFD();

  auto on_fd_received = task_runner_->CreateCheckpoint("on_fd_received");
  EXPECT_CALL(*cli_, OnFileDescriptorReceived(_))
      .WillOnce([on_fd_received](int fd) {
        char buf[sizeof(kFileContent)] = {};
        ASSERT_EQ(0, lseek(fd, 0, SEEK_SET));
        ASSERT_EQ(static_cast<int32_t>(sizeof(buf)),
                  PERFETTO_EINTR(read(fd, buf, sizeof(buf))));
        ASSERT_STREQ(kFileContent, buf);
        on_fd_received();
      });
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_));
  task_runner_->RunUntilCheckpoint("on_fd_received");
}

TEST_F(HostImplTest, ReceiveFileDescriptor) {
  auto received = task_runner_->CreateCheckpoint("received");
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  static constexpr char kFileContent[] = "shared file";
  RequestProto req_args;
  base::TempFile tx_file = base::TempFile::CreateUnlinked();
  ASSERT_EQ(static_cast<size_t>(base::WriteAll(tx_file.fd(), kFileContent,
                                               sizeof(kFileContent))),
            sizeof(kFileContent));
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args, false,
                     tx_file.fd());
  EXPECT_CALL(*cli_, OnInvokeMethodReply(_));
  base::ScopedFile rx_fd;
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce([received, &fake_service, &rx_fd](const RequestProto&,
                                                  DeferredBase*) {
        rx_fd = fake_service->TakeReceivedFD();
        received();
      });

  task_runner_->RunUntilCheckpoint("received");

  ASSERT_TRUE(rx_fd);
  char buf[sizeof(kFileContent)] = {};
  ASSERT_EQ(0, lseek(*rx_fd, 0, SEEK_SET));
  ASSERT_EQ(static_cast<int32_t>(sizeof(buf)),
            PERFETTO_EINTR(read(*rx_fd, buf, sizeof(buf))));
  ASSERT_STREQ(kFileContent, buf);
}
#endif  // !OS_WIN

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
      .WillOnce([on_host_method](const RequestProto& req, DeferredBase*) {
        ASSERT_EQ("foo", req.data());
        on_host_method();
      });
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
      .WillOnce(
          [on_invoke, &moved_reply](const RequestProto&, DeferredBase* reply) {
            moved_reply = std::move(*reply);
            on_invoke();
          });
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
      .WillOnce([on_reply_received](const Frame::InvokeMethodReply& reply) {
        ASSERT_TRUE(reply.success());
        ASSERT_FALSE(reply.has_more());
        ReplyProto reply_args;
        reply_args.ParseFromString(reply.reply_proto());
        ASSERT_EQ("bar", reply_args.data());
        on_reply_received();
      });
  task_runner_->RunUntilCheckpoint("on_reply_received");
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// Check ClientInfo of the service.
TEST_F(HostImplTest, ServiceClientInfo) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  RequestProto req_args;
  req_args.set_data("foo");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args);
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce([fake_service](const RequestProto& req, DeferredBase* reply) {
        ASSERT_EQ("foo", req.data());
        std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
        reply_args->set_data("bar");
        reply->Resolve(AsyncResult<ProtoMessage>(
            std::unique_ptr<ProtoMessage>(reply_args.release())));
        // Verifies the pid() and uid() values in ClientInfo.
        const auto& client_info = fake_service->client_info();
        ASSERT_EQ(client_info.uid(), getuid());
        ASSERT_EQ(client_info.pid(), getpid());
      });

  EXPECT_CALL(*cli_, OnInvokeMethodReply(_)).WillOnce(Return());
  task_runner_->RunUntilIdle();
}

TEST_F(HostImplTest, SetPeerIdentityUnixSocket) {
  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host_->ExposeService(std::unique_ptr<Service>(fake_service)));
  // SetPeerIdentity must be the first message. Use getpid()+1/geteuid+1 to
  // check that this message doesn't take effect for Unix socket.
  cli_->SetPeerIdentity(geteuid() + 1, getpid() + 1, "test_machine_id_hint");

  auto on_bind = task_runner_->CreateCheckpoint("on_bind");
  cli_->BindService("FakeService");
  EXPECT_CALL(*cli_, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner_->RunUntilCheckpoint("on_bind");

  RequestProto req_args;
  req_args.set_data("foo");
  cli_->InvokeMethod(cli_->last_bound_service_id_, 1, req_args);
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce([fake_service](const RequestProto& req, DeferredBase* reply) {
        ASSERT_EQ("foo", req.data());
        std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
        reply_args->set_data("bar");
        reply->Resolve(AsyncResult<ProtoMessage>(
            std::unique_ptr<ProtoMessage>(reply_args.release())));
        // Verifies the pid() and uid() values in ClientInfo.
        const auto& client_info = fake_service->client_info();
        ASSERT_EQ(client_info.uid(), getuid());
        ASSERT_EQ(client_info.pid(), getpid());
        ASSERT_EQ(client_info.machine_id(), base::kDefaultMachineID);
      });

  EXPECT_CALL(*cli_, OnInvokeMethodReply(_)).WillOnce(Return());
  task_runner_->RunUntilIdle();
}

TEST(HostImpl, SetPeerIdentityTcpSocket) {
  std::unique_ptr<base::TestTaskRunner> task_runner(new base::TestTaskRunner());
  std::unique_ptr<HostImpl> host_impl;
  std::unique_ptr<FakeClient> cli;

  auto tear_down = base::OnScopeExit([&]() {
    task_runner->RunUntilIdle();
    cli.reset();
    host_impl.reset();
    task_runner->RunUntilIdle();
    task_runner.reset();
  });

  Host* host = Host::CreateInstance("127.0.0.1:0", task_runner.get()).release();
  ASSERT_NE(nullptr, host);
  host_impl.reset(static_cast<HostImpl*>(host));

  auto sock_name = host_impl->sock()->GetSockAddr();
  cli.reset(new FakeClient(sock_name.c_str(), task_runner.get()));

  auto on_connect = task_runner->CreateCheckpoint("on_connect");
  EXPECT_CALL(*cli, OnConnect()).WillOnce(on_connect);
  task_runner->RunUntilCheckpoint("on_connect");

  FakeService* fake_service = new FakeService("FakeService");
  ASSERT_TRUE(host->ExposeService(std::unique_ptr<Service>(fake_service)));
  // Set peer identity with fake values.
  cli->SetPeerIdentity(123, 456, "test_machine_id_hint");

  auto on_bind = task_runner->CreateCheckpoint("on_bind");
  cli->BindService("FakeService");
  EXPECT_CALL(*cli, OnServiceBound(_)).WillOnce(InvokeWithoutArgs(on_bind));
  task_runner->RunUntilCheckpoint("on_bind");

  RequestProto req_args;
  req_args.set_data("foo");
  cli->InvokeMethod(cli->last_bound_service_id_, 1, req_args);
  EXPECT_CALL(*fake_service, OnFakeMethod1(_, _))
      .WillOnce([fake_service](const RequestProto& req, DeferredBase* reply) {
        ASSERT_EQ("foo", req.data());
        std::unique_ptr<ReplyProto> reply_args(new ReplyProto());
        reply_args->set_data("bar");
        reply->Resolve(AsyncResult<ProtoMessage>(
            std::unique_ptr<ProtoMessage>(reply_args.release())));
        // Verify peer identity.
        const auto& client_info = fake_service->client_info();
        ASSERT_EQ(client_info.uid(), 123u);
        ASSERT_EQ(client_info.pid(), 456);
        // ClientInfo contains non-default raw machine ID.
        ASSERT_NE(client_info.machine_id(), base::kDefaultMachineID);
      });

  EXPECT_CALL(*cli, OnInvokeMethodReply(_)).WillOnce(Return());
  task_runner->RunUntilIdle();
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||
        // PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

// TODO(primiano): add the tests below in next CLs.
// TEST(HostImplTest, ManyClients) {}
// TEST(HostImplTest, OverlappingRequstsOutOfOrder) {}
// TEST(HostImplTest, StreamingRequest) {}
// TEST(HostImplTest, ManyDropReplyRequestsDontLeakMemory) {}

}  // namespace
}  // namespace ipc
}  // namespace perfetto
