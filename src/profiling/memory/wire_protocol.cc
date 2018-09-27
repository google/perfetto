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

#include "src/profiling/memory/wire_protocol.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"

#include <sys/socket.h>
#include <sys/types.h>

namespace perfetto {

namespace {
template <typename T>
bool ViewAndAdvance(char** ptr, T** out, const char* end) {
  if (end - sizeof(T) < *ptr)
    return false;
  *out = reinterpret_cast<T*>(ptr);
  ptr += sizeof(T);
  return true;
}
}  // namespace

bool SendWireMessage(int sock, const WireMessage& msg) {
  uint64_t total_size;
  struct iovec iovecs[4] = {};
  // TODO(fmayer): Maye pack these two.
  iovecs[0].iov_base = &total_size;
  iovecs[0].iov_len = sizeof(total_size);
  iovecs[1].iov_base = const_cast<RecordType*>(&msg.record_type);
  iovecs[1].iov_len = sizeof(msg.record_type);
  if (msg.alloc_header) {
    iovecs[2].iov_base = msg.alloc_header;
    iovecs[2].iov_len = sizeof(*msg.alloc_header);
  } else if (msg.free_header) {
    iovecs[2].iov_base = msg.free_header;
    iovecs[2].iov_len = sizeof(*msg.free_header);
  } else {
    PERFETTO_DCHECK(false);
    return false;
  }

  iovecs[3].iov_base = msg.payload;
  iovecs[3].iov_len = msg.payload_size;

  struct msghdr hdr = {};
  hdr.msg_iov = iovecs;
  if (msg.payload) {
    hdr.msg_iovlen = base::ArraySize(iovecs);
    total_size = iovecs[1].iov_len + iovecs[2].iov_len + iovecs[3].iov_len;
  } else {
    // If we are not sending payload, just ignore that iovec.
    hdr.msg_iovlen = base::ArraySize(iovecs) - 1;
    total_size = iovecs[1].iov_len + iovecs[2].iov_len;
  }

  ssize_t sent = sendmsg(sock, &hdr, MSG_NOSIGNAL);
  return sent == static_cast<ssize_t>(total_size + sizeof(total_size));
}

bool ReceiveWireMessage(char* buf, size_t size, WireMessage* out) {
  RecordType* record_type;
  char* end = buf + size;
  if (!ViewAndAdvance<RecordType>(&buf, &record_type, end))
    return false;
  switch (*record_type) {
    case RecordType::Malloc:
      if (!ViewAndAdvance<AllocMetadata>(&buf, &out->alloc_header, end))
        return false;
      out->payload = buf;
      if (buf > end) {
        PERFETTO_DCHECK(false);
        return false;
      }
      out->payload_size = static_cast<size_t>(end - buf);
      break;
    case RecordType::Free:
      if (!ViewAndAdvance<FreeMetadata>(&buf, &out->free_header, end))
        return false;
      break;
  }
  out->record_type = *record_type;
  return true;
}

}  // namespace perfetto
