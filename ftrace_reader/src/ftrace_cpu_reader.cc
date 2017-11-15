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

#include "ftrace_reader/ftrace_cpu_reader.h"

#include <utility>

#include "base/logging.h"
#include "ftrace_to_proto_translation_table.h"

#include "protos/ftrace/ftrace_event.pbzero.h"

namespace perfetto {

namespace {

// For further documentation of these constants see the kernel source:
// linux/include/linux/ring_buffer.h
// Some information about the values of these constants are exposed to user
// space at: /sys/kernel/debug/tracing/events/header_event
const uint32_t kTypeDataTypeLengthMax = 28;
const uint32_t kTypePadding = 29;
const uint32_t kTypeTimeExtend = 30;
const uint32_t kTypeTimeStamp = 31;

const size_t kPageSize = 4096;

struct PageHeader {
  uint64_t timestamp;
  uint32_t size;
  uint32_t : 24;
  uint32_t overwrite : 8;
};

struct EventHeader {
  uint32_t type_or_length : 5;
  uint32_t time_delta : 27;
};

struct TimeStamp {
  uint64_t tv_nsec;
  uint64_t tv_sec;
};

}  // namespace

FtraceCpuReader::FtraceCpuReader(const FtraceToProtoTranslationTable* table,
                                 size_t cpu,
                                 base::ScopedFile fd)
    : table_(table), cpu_(cpu), fd_(std::move(fd)) {}

bool FtraceCpuReader::Read(const Config&, pbzero::FtraceEventBundle* bundle) {
  if (!fd_)
    return false;

  uint8_t* buffer = GetBuffer();
  // TOOD(hjd): One read() per page may be too many.
  long bytes = PERFETTO_EINTR(read(fd_.get(), buffer, kPageSize));
  if (bytes == -1 || bytes == 0)
    return false;
  PERFETTO_CHECK(bytes <= kPageSize);

  return ParsePage(cpu_, buffer, bytes, bundle);
}

FtraceCpuReader::~FtraceCpuReader() = default;
FtraceCpuReader::FtraceCpuReader(FtraceCpuReader&&) = default;

uint8_t* FtraceCpuReader::GetBuffer() {
  // TODO(primiano): Guard against overflows, like BufferedFrameDeserializer.
  if (!buffer_)
    buffer_ = std::unique_ptr<uint8_t[]>(new uint8_t[kPageSize]);
  return buffer_.get();
}

// The structure of a raw trace buffer page is as follows:
// First a page header:
//   8 bytes of timestamp
//   8 bytes of page length TODO(hjd): other fields also defined here?
// // TODO(hjd): Document rest of format.
// Some information about the layout of the page header is available in user
// space at: /sys/kernel/debug/tracing/events/header_event
// This method is deliberately static so it can be tested independently.
bool FtraceCpuReader::ParsePage(size_t cpu,
                                const uint8_t* ptr,
                                size_t size,
                                pbzero::FtraceEventBundle* bundle) {
  const uint8_t* const start = ptr;
  const uint8_t* const end = ptr + size;
  bundle->set_cpu(cpu);

  // TODO(hjd): Read this format dynamically?
  PageHeader page_header;
  if (!ReadAndAdvance(&ptr, end, &page_header))
    return false;
  if (ptr + page_header.size > end)
    return false;

  // TODO(hjd): Remove.
  (void)start;

  while (ptr < end) {
    EventHeader event_header;
    if (!ReadAndAdvance(&ptr, end, &event_header))
      return false;
    switch (event_header.type_or_length) {
      case kTypePadding: {
        // Left over page padding or discarded event.
        PERFETTO_DLOG("Padding");
        if (event_header.time_delta == 0) {
          // TODO(hjd): Look at the next few bytes for read size;
        }
        PERFETTO_CHECK(false);  // TODO(hjd): Handle
        break;
      }
      case kTypeTimeExtend: {
        // Extend the time delta.
        PERFETTO_DLOG("Extended Time Delta");
        uint32_t time_delta_ext;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &time_delta_ext))
          return false;
        (void)time_delta_ext;
        // TODO(hjd): Handle.
        break;
      }
      case kTypeTimeStamp: {
        // Sync time stamp with external clock.
        PERFETTO_DLOG("Time Stamp");
        TimeStamp time_stamp;
        if (!ReadAndAdvance<TimeStamp>(&ptr, end, &time_stamp))
          return false;
        // TODO(hjd): Handle.
        break;
      }
      // Data record:
      default: {
        PERFETTO_CHECK(event_header.type_or_length <= kTypeDataTypeLengthMax);
        // type_or_length is <=28 so it represents the length of a data record.
        if (event_header.type_or_length == 0) {
          // TODO(hjd): Look at the next few bytes for real size.
          PERFETTO_CHECK(false);
        }
        const uint8_t* next = ptr + 4 * event_header.type_or_length;

        uint16_t event_type;
        if (!ReadAndAdvance<uint16_t>(&ptr, end, &event_type))
          return false;

        // Common headers:
        // TODO(hjd): Read this format dynamically?
        uint8_t flags;
        uint8_t preempt_count;
        uint32_t pid;
        if (!ReadAndAdvance<uint8_t>(&ptr, end, &flags))
          return false;
        if (!ReadAndAdvance<uint8_t>(&ptr, end, &preempt_count))
          return false;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &pid))
          return false;

        PERFETTO_DLOG("Event type=%d pid=%d", event_type, pid);

        pbzero::FtraceEvent* event = bundle->add_event();
        event->set_pid(pid);

        if (event_type == 5) {
          // Trace Marker Parser
          uint64_t ip;
          if (!ReadAndAdvance<uint64_t>(&ptr, end, &ip))
            return false;

          const uint8_t* word_start = ptr;
          PERFETTO_DLOG("  marker=%s", word_start);
          while (*ptr != '\0')
            ptr++;
        }

        // Jump to next event.
        ptr = next;
        PERFETTO_DLOG("%zu", ptr - start);
      }
    }
  }
  return true;
}

}  // namespace perfetto
