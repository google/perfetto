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
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"

#include <sys/socket.h>
#include <sys/types.h>

namespace perfetto {
namespace profiling {

namespace {
template <typename T>
bool ViewAndAdvance(char** ptr, T** out, const char* end) {
  if (end - sizeof(T) < *ptr)
    return false;
  *out = reinterpret_cast<T*>(*ptr);
  *ptr += sizeof(T);
  return true;
}

// We need this to prevent crashes due to FORTIFY_SOURCE.
void UnsafeMemcpy(char* dest, const char* src, size_t n)
    __attribute__((no_sanitize("address", "hwaddress"))) {
  for (size_t i = 0; i < n; ++i) {
    dest[i] = src[i];
  }
}
}  // namespace

int64_t SendWireMessage(SharedRingBuffer* shmem, const WireMessage& msg) {
  switch (msg.record_type) {
    case RecordType::Malloc: {
      PERFETTO_DCHECK(msg.free_header == nullptr);
      PERFETTO_DCHECK(msg.alloc_header != nullptr);
      size_t total_size = sizeof(msg.record_type) + sizeof(*msg.alloc_header) +
                          msg.payload_size;
      SharedRingBuffer::Buffer buf;
      {
        ScopedSpinlock lock = shmem->AcquireLock(ScopedSpinlock::Mode::Try);
        if (!lock.locked()) {
          PERFETTO_DLOG("Failed to acquire spinlock.");
          errno = EAGAIN;
          return -1;
        }
        buf = shmem->BeginWrite(lock, total_size);
      }
      if (!buf) {
        PERFETTO_DLOG("Buffer overflow.");
        shmem->EndWrite(std::move(buf));
        errno = EAGAIN;
        return -1;
      }

      memcpy(buf.data, &msg.record_type, sizeof(msg.record_type));
      memcpy(buf.data + sizeof(msg.record_type), msg.alloc_header,
             sizeof(*msg.alloc_header));
      UnsafeMemcpy(reinterpret_cast<char*>(buf.data) + sizeof(msg.record_type) +
                       sizeof(*msg.alloc_header),
                   msg.payload, msg.payload_size);
      auto bytes_free = buf.bytes_free;
      shmem->EndWrite(std::move(buf));
      return static_cast<int64_t>(bytes_free);
    }
    case RecordType::Free: {
      PERFETTO_DCHECK(msg.free_header != nullptr);
      PERFETTO_DCHECK(msg.alloc_header == nullptr);
      PERFETTO_DCHECK(msg.payload == nullptr);
      PERFETTO_DCHECK(msg.payload_size == 0);
      constexpr size_t total_size =
          sizeof(msg.record_type) + sizeof(*msg.free_header);
      SharedRingBuffer::Buffer buf;
      {
        ScopedSpinlock lock = shmem->AcquireLock(ScopedSpinlock::Mode::Try);
        if (!lock.locked()) {
          PERFETTO_DLOG("Failed to acquire spinlock.");
          errno = EAGAIN;
          return -1;
        }
        buf = shmem->BeginWrite(lock, total_size);
      }
      if (!buf) {
        PERFETTO_DLOG("Buffer overflow.");
        shmem->EndWrite(std::move(buf));
        errno = EAGAIN;
        return -1;
      }

      memcpy(buf.data, &msg.record_type, sizeof(msg.record_type));
      memcpy(buf.data + sizeof(msg.record_type), msg.free_header,
             sizeof(*msg.free_header));
      auto bytes_free = buf.bytes_free;
      shmem->EndWrite(std::move(buf));
      return static_cast<int64_t>(bytes_free);
    }
  }
}

bool ReceiveWireMessage(char* buf, size_t size, WireMessage* out) {
  RecordType* record_type;
  char* end = buf + size;
  if (!ViewAndAdvance<RecordType>(&buf, &record_type, end)) {
    PERFETTO_DFATAL_OR_ELOG("Cannot read record type.");
    return false;
  }

  out->payload = nullptr;
  out->payload_size = 0;
  out->record_type = *record_type;

  if (*record_type == RecordType::Malloc) {
    if (!ViewAndAdvance<AllocMetadata>(&buf, &out->alloc_header, end)) {
      PERFETTO_DFATAL_OR_ELOG("Cannot read alloc header.");
      return false;
    }
    out->payload = buf;
    if (buf > end) {
      PERFETTO_DFATAL_OR_ELOG("Receive buffer overflowed");
      return false;
    }
    out->payload_size = static_cast<size_t>(end - buf);
  } else if (*record_type == RecordType::Free) {
    if (!ViewAndAdvance<FreeEntry>(&buf, &out->free_header, end)) {
      PERFETTO_DFATAL_OR_ELOG("Cannot read free header.");
      return false;
    }
  } else {
    PERFETTO_DFATAL_OR_ELOG("Invalid record type.");
    return false;
  }
  return true;
}

}  // namespace profiling
}  // namespace perfetto
