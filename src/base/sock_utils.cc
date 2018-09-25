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

#include "perfetto/base/sock_utils.h"

#include <sys/socket.h>
#include <sys/un.h>

namespace perfetto {
namespace base {
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
}

// The CMSG_* macros use NULL instead of nullptr.
#pragma GCC diagnostic push
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif

ssize_t Send(int fd,
             const void* msg,
             size_t len,
             const int* send_fds,
             size_t num_fds) {
  msghdr msg_hdr = {};
  iovec iov = {const_cast<void*>(msg), len};
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  alignas(cmsghdr) char control_buf[256];

  if (num_fds > 0) {
    const CBufLenType control_buf_len =
        static_cast<CBufLenType>(CMSG_SPACE(num_fds * sizeof(int)));
    PERFETTO_CHECK(control_buf_len <= sizeof(control_buf));
    memset(control_buf, 0, sizeof(control_buf));
    msg_hdr.msg_control = control_buf;
    msg_hdr.msg_controllen = control_buf_len;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_hdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = static_cast<CBufLenType>(CMSG_LEN(num_fds * sizeof(int)));
    memcpy(CMSG_DATA(cmsg), send_fds, num_fds * sizeof(int));
    msg_hdr.msg_controllen = cmsg->cmsg_len;
  }

  return PERFETTO_EINTR(sendmsg(fd, &msg_hdr, kNoSigPipe));
}

ssize_t Receive(int fd,
                void* msg,
                size_t len,
                base::ScopedFile* fd_vec,
                size_t max_files) {
  msghdr msg_hdr = {};
  iovec iov = {msg, len};
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  alignas(cmsghdr) char control_buf[256];

  if (max_files > 0) {
    msg_hdr.msg_control = control_buf;
    msg_hdr.msg_controllen =
        static_cast<CBufLenType>(CMSG_SPACE(max_files * sizeof(int)));
    PERFETTO_CHECK(msg_hdr.msg_controllen <= sizeof(control_buf));
  }
  const ssize_t sz = PERFETTO_EINTR(recvmsg(fd, &msg_hdr, kNoSigPipe));
  if (sz <= 0) {
    return sz;
  }
  PERFETTO_CHECK(static_cast<size_t>(sz) <= len);

  int* fds = nullptr;
  uint32_t fds_len = 0;

  if (max_files > 0) {
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_hdr); cmsg;
         cmsg = CMSG_NXTHDR(&msg_hdr, cmsg)) {
      const size_t payload_len = cmsg->cmsg_len - CMSG_LEN(0);
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        PERFETTO_DCHECK(payload_len % sizeof(int) == 0u);
        PERFETTO_CHECK(fds == nullptr);
        fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        fds_len = static_cast<uint32_t>(payload_len / sizeof(int));
      }
    }
  }

  if (msg_hdr.msg_flags & MSG_TRUNC || msg_hdr.msg_flags & MSG_CTRUNC) {
    for (size_t i = 0; fds && i < fds_len; ++i)
      close(fds[i]);
    errno = EMSGSIZE;
    return -1;
  }

  for (size_t i = 0; fds && i < fds_len; ++i) {
    if (i < max_files)
      fd_vec[i].reset(fds[i]);
    else
      close(fds[i]);
  }

  return sz;
}

#pragma GCC diagnostic pop

}  // namespace base
}  // namespace perfetto
