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

#include "cpu_reader.h"

#include <utility>

#include "perfetto/base/logging.h"
#include "proto_translation_table.h"

#include "protos/ftrace/ftrace_event.pbzero.h"
#include "protos/ftrace/print.pbzero.h"
#include "protos/ftrace/sched_switch.pbzero.h"

#include "protos/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {

namespace {

using BundleHandle =
    protozero::ProtoZeroMessageHandle<protos::pbzero::FtraceEventBundle>;

const std::vector<bool> BuildEnabledVector(const ProtoTranslationTable& table,
                                           const std::set<std::string>& names) {
  std::vector<bool> enabled(table.largest_id() + 1);
  for (const std::string& name : names) {
    const Event* event = table.GetEventByName(name);
    if (!event)
      continue;
    enabled[event->ftrace_event_id] = true;
  }
  return enabled;
}

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

EventFilter::EventFilter(const ProtoTranslationTable& table,
                         std::set<std::string> names)
    : enabled_ids_(BuildEnabledVector(table, names)),
      enabled_names_(std::move(names)) {}
EventFilter::~EventFilter() = default;

CpuReader::CpuReader(const ProtoTranslationTable* table,
                     size_t cpu,
                     base::ScopedFile fd)
    : table_(table), cpu_(cpu), fd_(std::move(fd)) {}

int CpuReader::GetFileDescriptor() {
  return fd_.get();
}

bool CpuReader::Drain(const std::array<const EventFilter*, kMaxSinks>& filters,
                      const std::array<BundleHandle, kMaxSinks>& bundles) {
  if (!fd_)
    return false;

  uint8_t* buffer = GetBuffer();
  // TOOD(hjd): One read() per page may be too many.
  long bytes = PERFETTO_EINTR(read(fd_.get(), buffer, kPageSize));
  if (bytes != kPageSize)
    return false;

  for (size_t i = 0; i < kMaxSinks; i++) {
    if (!filters[i])
      break;
    bool result = ParsePage(cpu_, buffer, filters[i], &*bundles[i], table_);
    PERFETTO_DCHECK(result);
  }
  return true;
}

CpuReader::~CpuReader() = default;

uint8_t* CpuReader::GetBuffer() {
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
bool CpuReader::ParsePage(size_t cpu,
                          const uint8_t* ptr,
                          const EventFilter* filter,
                          protos::pbzero::FtraceEventBundle* bundle,
                          const ProtoTranslationTable* table) {
  // TODO(hjd): Remove when the generic parser comes in.
  const size_t print_id = table->GetEventByName("print")->ftrace_event_id;
  const size_t sched_switch_id =
      table->GetEventByName("sched_switch")->ftrace_event_id;

  const uint8_t* const start_of_page = ptr;
  const uint8_t* const end_of_page = ptr + kPageSize;

  bundle->set_cpu(cpu);

  (void)start_of_page;

  // TODO(hjd): Read this format dynamically?
  PageHeader page_header;
  if (!ReadAndAdvance(&ptr, end_of_page, &page_header))
    return false;

  // TODO(hjd): There is something wrong with the page header struct.
  page_header.size = page_header.size & 0xfffful;

  const uint8_t* const end = ptr + page_header.size;
  if (end > end_of_page)
    return false;

  uint64_t timestamp = page_header.timestamp;

  while (ptr < end) {
    EventHeader event_header;
    if (!ReadAndAdvance(&ptr, end, &event_header))
      return false;

    timestamp += event_header.time_delta;

    switch (event_header.type_or_length) {
      case kTypePadding: {
        // Left over page padding or discarded event.
        if (event_header.time_delta == 0) {
          // TODO(hjd): Look at the next few bytes for read size;
          PERFETTO_CHECK(false);  // TODO(hjd): Handle
        }
        uint32_t length;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &length))
          return false;
        ptr += length;
        break;
      }
      case kTypeTimeExtend: {
        // Extend the time delta.
        uint32_t time_delta_ext;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &time_delta_ext))
          return false;
        // See https://goo.gl/CFBu5x
        timestamp += ((uint64_t)time_delta_ext) << 27;
        break;
      }
      case kTypeTimeStamp: {
        // Sync time stamp with external clock.
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

        uint16_t ftrace_event_id;
        if (!ReadAndAdvance<uint16_t>(&ptr, end, &ftrace_event_id))
          return false;
        if (!filter->IsEventEnabled(ftrace_event_id)) {
          ptr = next;
          break;
        }

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

        // PERFETTO_DLOG("Event type=%d pid=%d", ftrace_event_id, pid);

        protos::pbzero::FtraceEvent* event = bundle->add_event();
        event->set_pid(pid);
        event->set_timestamp(timestamp);

        // TODO(hjd): Replace this handrolled code with generic parsing code.
        if (ftrace_event_id == print_id) {
          protos::pbzero::PrintFtraceEvent* print_event = event->set_print();
          // Trace Marker Parser
          uint64_t ip;
          if (!ReadAndAdvance<uint64_t>(&ptr, end, &ip))
            return false;
          print_event->set_ip(ip);

          // TODO(hjd): Not sure if this is null-terminated.
          const uint8_t* buf_start = ptr;
          const uint8_t* buf_end = next;
          for (const uint8_t* c = buf_start; c < buf_end; c++) {
            if (*c != '\0')
              continue;
            print_event->set_buf(reinterpret_cast<const char*>(buf_start),
                                 c - buf_start);
            break;
          }
          print_event->Finalize();
        }

        // TODO(hjd): Replace this handrolled code with generic parsing code.
        if (ftrace_event_id == sched_switch_id) {
          protos::pbzero::SchedSwitchFtraceEvent* switch_event =
              event->set_sched_switch();

          char prev_comm[16];
          uint32_t prev_pid;
          uint32_t prev_prio;
          uint64_t prev_state;
          char next_comm[16];
          uint32_t next_pid;
          uint32_t next_prio;

          // TODO(hjd): Avoid this copy.
          if (!ReadAndAdvance<char[16]>(&ptr, end, &prev_comm))
            return false;
          if (!ReadAndAdvance<uint32_t>(&ptr, end, &prev_pid))
            return false;
          if (!ReadAndAdvance<uint32_t>(&ptr, end, &prev_prio))
            return false;
          if (!ReadAndAdvance<uint64_t>(&ptr, end, &prev_state))
            return false;
          if (!ReadAndAdvance<char[16]>(&ptr, end, &next_comm))
            return false;
          if (!ReadAndAdvance<uint32_t>(&ptr, end, &next_pid))
            return false;
          if (!ReadAndAdvance<uint32_t>(&ptr, end, &next_prio))
            return false;
          // TODO(hjd): Not sure if this is null-terminated.
          prev_comm[15] = '\0';
          switch_event->set_prev_comm(prev_comm);
          switch_event->set_prev_pid(prev_pid);
          switch_event->set_prev_prio(prev_prio);
          switch_event->set_prev_state(prev_state);
          // TODO(hjd): Not sure if this is null-terminated.
          next_comm[15] = '\0';
          switch_event->set_next_comm(next_comm);
          switch_event->set_next_pid(next_pid);
          switch_event->set_next_prio(next_prio);
          switch_event->Finalize();
        }

        event->Finalize();

        // Jump to next event.
        ptr = next;
      }
    }
  }
  return true;
}

}  // namespace perfetto
