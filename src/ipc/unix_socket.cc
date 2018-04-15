/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/ipc/unix_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <memory>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#include <sys/ucred.h>
#endif

namespace perfetto {
namespace ipc {

// TODO(primiano): Add ThreadChecker to methods of this class.

namespace {

// MSG_NOSIGNAL is not supported on Mac OS X, but in that case the socket is
// created with SO_NOSIGPIPE (See InitializeSocket()).
#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
constexpr int kNoSigPipe = 0;
#else
constexpr int kNoSigPipe = MSG_NOSIGNAL;
#endif

// Android takes an int instead of socklen_t for the control buffer size.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
using CBufLenType = size_t;
#else
using CBufLenType = socklen_t;
#endif

bool MakeSockAddr(const std::string& socket_name,
                  sockaddr_un* addr,
                  socklen_t* addr_size) {
  memset(addr, 0, sizeof(*addr));
  const size_t name_len = socket_name.size();
  if (name_len >= sizeof(addr->sun_path)) {
    errno = ENAMETOOLONG;
    return false;
  }
  memcpy(addr->sun_path, socket_name.data(), name_len);
  if (addr->sun_path[0] == '@')
    addr->sun_path[0] = '\0';
  addr->sun_family = AF_UNIX;
  *addr_size = static_cast<socklen_t>(
      __builtin_offsetof(sockaddr_un, sun_path) + name_len + 1);
  return true;
}

base::ScopedFile CreateSocket() {
  return base::ScopedFile(socket(AF_UNIX, SOCK_STREAM, 0));
}

}  // namespace

// static
base::ScopedFile UnixSocket::CreateAndBind(const std::string& socket_name) {
  base::ScopedFile fd = CreateSocket();
  if (!fd)
    return fd;

  sockaddr_un addr;
  socklen_t addr_size;
  if (!MakeSockAddr(socket_name, &addr, &addr_size)) {
    return base::ScopedFile();
  }

  if (bind(*fd, reinterpret_cast<sockaddr*>(&addr), addr_size)) {
    PERFETTO_DPLOG("bind()");
    return base::ScopedFile();
  }

  return fd;
}

// static
std::unique_ptr<UnixSocket> UnixSocket::Listen(const std::string& socket_name,
                                               EventListener* event_listener,
                                               base::TaskRunner* task_runner) {
  // Forward the call to the Listen() overload below.
  return Listen(CreateAndBind(socket_name), event_listener, task_runner);
}

// static
std::unique_ptr<UnixSocket> UnixSocket::Listen(base::ScopedFile socket_fd,
                                               EventListener* event_listener,
                                               base::TaskRunner* task_runner) {
  std::unique_ptr<UnixSocket> sock(new UnixSocket(
      event_listener, task_runner, std::move(socket_fd), State::kListening));
  return sock;
}

// static
std::unique_ptr<UnixSocket> UnixSocket::Connect(const std::string& socket_name,
                                                EventListener* event_listener,
                                                base::TaskRunner* task_runner) {
  std::unique_ptr<UnixSocket> sock(new UnixSocket(event_listener, task_runner));
  sock->DoConnect(socket_name);
  return sock;
}

UnixSocket::UnixSocket(EventListener* event_listener,
                       base::TaskRunner* task_runner)
    : UnixSocket(event_listener,
                 task_runner,
                 base::ScopedFile(),
                 State::kDisconnected) {}

UnixSocket::UnixSocket(EventListener* event_listener,
                       base::TaskRunner* task_runner,
                       base::ScopedFile adopt_fd,
                       State adopt_state)
    : event_listener_(event_listener),
      task_runner_(task_runner),
      weak_ptr_factory_(this) {
  state_ = State::kDisconnected;
  if (adopt_state == State::kDisconnected) {
    // We get here from the default ctor().
    PERFETTO_DCHECK(!adopt_fd);
    fd_ = CreateSocket();
    if (!fd_) {
      last_error_ = errno;
      return;
    }
  } else if (adopt_state == State::kConnected) {
    // We get here from OnNewIncomingConnection().
    PERFETTO_DCHECK(adopt_fd);
    fd_ = std::move(adopt_fd);
    state_ = State::kConnected;
    ReadPeerCredentials();
  } else if (adopt_state == State::kListening) {
    // We get here from Listen().

    // |adopt_fd| might genuinely be invalid if the bind() failed.
    if (!adopt_fd) {
      last_error_ = errno;
      return;
    }

    fd_ = std::move(adopt_fd);
    if (listen(*fd_, SOMAXCONN)) {
      last_error_ = errno;
      PERFETTO_DPLOG("listen()");
      return;
    }
    state_ = State::kListening;
  } else {
    PERFETTO_CHECK(false);  // Unfeasible.
  }

  PERFETTO_DCHECK(fd_);
  last_error_ = 0;

#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
  const int no_sigpipe = 1;
  setsockopt(*fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  // There is no reason why a socket should outlive the process in case of
  // exec() by default, this is just working around a broken unix design.
  int fcntl_res = fcntl(*fd_, F_SETFD, FD_CLOEXEC);
  PERFETTO_CHECK(fcntl_res == 0);

  SetBlockingIO(false);

  base::WeakPtr<UnixSocket> weak_ptr = weak_ptr_factory_.GetWeakPtr();
  task_runner_->AddFileDescriptorWatch(*fd_, [weak_ptr]() {
    if (weak_ptr)
      weak_ptr->OnEvent();
  });
}

UnixSocket::~UnixSocket() {
  // The implicit dtor of |weak_ptr_factory_| will no-op pending callbacks.
  Shutdown(true);
}

// Called only by the Connect() static constructor.
void UnixSocket::DoConnect(const std::string& socket_name) {
  PERFETTO_DCHECK(state_ == State::kDisconnected);

  // This is the only thing that can gracefully fail in the ctor.
  if (!fd_)
    return NotifyConnectionState(false);

  sockaddr_un addr;
  socklen_t addr_size;
  if (!MakeSockAddr(socket_name, &addr, &addr_size)) {
    last_error_ = errno;
    return NotifyConnectionState(false);
  }

  int res = PERFETTO_EINTR(
      connect(*fd_, reinterpret_cast<sockaddr*>(&addr), addr_size));
  if (res && errno != EINPROGRESS) {
    last_error_ = errno;
    return NotifyConnectionState(false);
  }

  // At this point either |res| == 0 (the connect() succeeded) or started
  // asynchronously (EINPROGRESS).
  last_error_ = 0;
  state_ = State::kConnecting;

  // Even if the socket is non-blocking, connecting to a UNIX socket can be
  // acknowledged straight away rather than returning EINPROGRESS. In this case
  // just trigger an OnEvent without waiting for the FD watch. That will poll
  // the SO_ERROR and evolve the state into either kConnected or kDisconnected.
  if (res == 0) {
    base::WeakPtr<UnixSocket> weak_ptr = weak_ptr_factory_.GetWeakPtr();
    task_runner_->PostTask([weak_ptr]() {
      if (weak_ptr)
        weak_ptr->OnEvent();
    });
  }
}

void UnixSocket::ReadPeerCredentials() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  struct ucred user_cred;
  socklen_t len = sizeof(user_cred);
  int res = getsockopt(*fd_, SOL_SOCKET, SO_PEERCRED, &user_cred, &len);
  PERFETTO_CHECK(res == 0);
  peer_uid_ = user_cred.uid;
#else
  struct xucred user_cred;
  socklen_t len = sizeof(user_cred);
  int res = getsockopt(*fd_, 0, LOCAL_PEERCRED, &user_cred, &len);
  PERFETTO_CHECK(res == 0 && user_cred.cr_version == XUCRED_VERSION);
  peer_uid_ = static_cast<uid_t>(user_cred.cr_uid);
#endif
}

void UnixSocket::OnEvent() {
  if (state_ == State::kDisconnected)
    return;  // Some spurious event, typically queued just before Shutdown().

  if (state_ == State::kConnected)
    return event_listener_->OnDataAvailable(this);

  if (state_ == State::kConnecting) {
    PERFETTO_DCHECK(fd_);
    int sock_err = EINVAL;
    socklen_t err_len = sizeof(sock_err);
    int res = getsockopt(*fd_, SOL_SOCKET, SO_ERROR, &sock_err, &err_len);
    if (res == 0 && sock_err == EINPROGRESS)
      return;  // Not connected yet, just a spurious FD watch wakeup.
    if (res == 0 && sock_err == 0) {
      ReadPeerCredentials();
      state_ = State::kConnected;
      return event_listener_->OnConnect(this, true /* connected */);
    }
    last_error_ = sock_err;
    Shutdown(false);
    return event_listener_->OnConnect(this, false /* connected */);
  }

  // New incoming connection.
  if (state_ == State::kListening) {
    // There could be more than one incoming connection behind each FD watch
    // notification. Drain'em all.
    for (;;) {
      sockaddr_un cli_addr = {};
      socklen_t size = sizeof(cli_addr);
      base::ScopedFile new_fd(PERFETTO_EINTR(
          accept(*fd_, reinterpret_cast<sockaddr*>(&cli_addr), &size)));
      if (!new_fd)
        return;
      std::unique_ptr<UnixSocket> new_sock(new UnixSocket(
          event_listener_, task_runner_, std::move(new_fd), State::kConnected));
      event_listener_->OnNewIncomingConnection(this, std::move(new_sock));
    }
  }
}

bool UnixSocket::Send(const std::string& msg) {
  return Send(msg.c_str(), msg.size() + 1);
}

bool UnixSocket::Send(const void* msg,
                      size_t len,
                      int send_fd,
                      BlockingMode blocking_mode) {
  if (state_ != State::kConnected) {
    errno = last_error_ = ENOTCONN;
    return false;
  }

  msghdr msg_hdr = {};
  iovec iov = {const_cast<void*>(msg), len};
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  alignas(cmsghdr) char control_buf[256];

  if (send_fd > -1) {
    const CBufLenType control_buf_len =
        static_cast<CBufLenType>(CMSG_SPACE(sizeof(int)));
    PERFETTO_CHECK(control_buf_len <= sizeof(control_buf));
    memset(control_buf, 0, sizeof(control_buf));
    msg_hdr.msg_control = control_buf;
    msg_hdr.msg_controllen = control_buf_len;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_hdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(int));
    msg_hdr.msg_controllen = cmsg->cmsg_len;
  }

  if (blocking_mode == BlockingMode::kBlocking)
    SetBlockingIO(true);
  const ssize_t sz = PERFETTO_EINTR(sendmsg(*fd_, &msg_hdr, kNoSigPipe));
  if (blocking_mode == BlockingMode::kBlocking)
    SetBlockingIO(false);

  if (sz == static_cast<ssize_t>(len)) {
    last_error_ = 0;
    return true;
  }

  // If sendmsg() succeds but the returned size is < |len| it means that the
  // endpoint disconnected in the middle of the read, and we managed to send
  // only a portion of the buffer. In this case we should just give up.

  if (sz < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    // A genuine out-of-buffer. The client should retry or give up.
    // Man pages specify that EAGAIN and EWOULDBLOCK have the same semantic here
    // and clients should check for both.
    last_error_ = EAGAIN;
    return false;
  }

  // Either the the other endpoint disconnect (ECONNRESET) or some other error
  // happened.
  last_error_ = errno;
  PERFETTO_DPLOG("sendmsg() failed");
  Shutdown(true);
  return false;
}

void UnixSocket::Shutdown(bool notify) {
  base::WeakPtr<UnixSocket> weak_ptr = weak_ptr_factory_.GetWeakPtr();
  if (notify) {
    if (state_ == State::kConnected) {
      task_runner_->PostTask([weak_ptr]() {
        if (weak_ptr)
          weak_ptr->event_listener_->OnDisconnect(weak_ptr.get());
      });
    } else if (state_ == State::kConnecting) {
      task_runner_->PostTask([weak_ptr]() {
        if (weak_ptr)
          weak_ptr->event_listener_->OnConnect(weak_ptr.get(), false);
      });
    }
  }

  if (fd_) {
    shutdown(*fd_, SHUT_RDWR);
    task_runner_->RemoveFileDescriptorWatch(*fd_);
    fd_.reset();
  }
  state_ = State::kDisconnected;
}

size_t UnixSocket::Receive(void* msg, size_t len, base::ScopedFile* recv_fd) {
  if (state_ != State::kConnected) {
    last_error_ = ENOTCONN;
    return 0;
  }

  msghdr msg_hdr = {};
  iovec iov = {msg, len};
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  alignas(cmsghdr) char control_buf[256];

  if (recv_fd) {
    msg_hdr.msg_control = control_buf;
    msg_hdr.msg_controllen = static_cast<CBufLenType>(CMSG_SPACE(sizeof(int)));
    PERFETTO_CHECK(msg_hdr.msg_controllen <= sizeof(control_buf));
  }
  const ssize_t sz = PERFETTO_EINTR(recvmsg(*fd_, &msg_hdr, kNoSigPipe));
  if (sz < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    last_error_ = EAGAIN;
    return 0;
  }
  if (sz <= 0) {
    last_error_ = errno;
    Shutdown(true);
    return 0;
  }
  PERFETTO_CHECK(static_cast<size_t>(sz) <= len);

  int* fds = nullptr;
  uint32_t fds_len = 0;

  if (msg_hdr.msg_controllen > 0) {
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_hdr); cmsg;
         cmsg = CMSG_NXTHDR(&msg_hdr, cmsg)) {
      const size_t payload_len = cmsg->cmsg_len - CMSG_LEN(0);
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        PERFETTO_DCHECK(payload_len % sizeof(int) == 0u);
        PERFETTO_DCHECK(fds == nullptr);
        fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        fds_len = static_cast<uint32_t>(payload_len / sizeof(int));
      }
    }
  }

  if (msg_hdr.msg_flags & MSG_TRUNC || msg_hdr.msg_flags & MSG_CTRUNC) {
    for (size_t i = 0; fds && i < fds_len; ++i)
      close(fds[i]);
    last_error_ = EMSGSIZE;
    Shutdown(true);
    return 0;
  }

  for (size_t i = 0; fds && i < fds_len; ++i) {
    if (recv_fd && i == 0) {
      recv_fd->reset(fds[i]);
    } else {
      close(fds[i]);
    }
  }

  last_error_ = 0;
  return static_cast<size_t>(sz);
}

std::string UnixSocket::ReceiveString(size_t max_length) {
  std::unique_ptr<char[]> buf(new char[max_length + 1]);
  size_t rsize = Receive(buf.get(), max_length);
  PERFETTO_CHECK(static_cast<size_t>(rsize) <= max_length);
  buf[static_cast<size_t>(rsize)] = '\0';
  return std::string(buf.get());
}

void UnixSocket::NotifyConnectionState(bool success) {
  if (!success)
    Shutdown(false);

  base::WeakPtr<UnixSocket> weak_ptr = weak_ptr_factory_.GetWeakPtr();
  task_runner_->PostTask([weak_ptr, success]() {
    if (weak_ptr)
      weak_ptr->event_listener_->OnConnect(weak_ptr.get(), success);
  });
}

void UnixSocket::SetBlockingIO(bool is_blocking) {
  int flags = fcntl(*fd_, F_GETFL, 0);
  if (!is_blocking) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~static_cast<int>(O_NONBLOCK);
  }
  bool fcntl_res = fcntl(fd(), F_SETFL, flags);
  PERFETTO_CHECK(fcntl_res == 0);
}

UnixSocket::EventListener::~EventListener() {}
void UnixSocket::EventListener::OnNewIncomingConnection(
    UnixSocket*,
    std::unique_ptr<UnixSocket>) {}
void UnixSocket::EventListener::OnConnect(UnixSocket*, bool) {}
void UnixSocket::EventListener::OnDisconnect(UnixSocket*) {}
void UnixSocket::EventListener::OnDataAvailable(UnixSocket*) {}

}  // namespace ipc
}  // namespace perfetto
