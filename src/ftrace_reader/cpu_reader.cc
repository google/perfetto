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
        const uint8_t* start = ptr;
        const uint8_t* next = ptr + 4 * event_header.type_or_length;

        uint16_t ftrace_event_id;
        if (!ReadAndAdvance<uint16_t>(&ptr, end, &ftrace_event_id))
          return false;
        if (filter->IsEventEnabled(ftrace_event_id)) {
          protos::pbzero::FtraceEvent* event = bundle->add_event();
          event->set_timestamp(timestamp);
          if (!ParseEvent(ftrace_event_id, start, next, table, event))
            return false;
        }

        // Jump to next event.
        ptr = next;
      }
    }
  }
  return true;
}

bool CpuReader::ParseEvent(uint16_t ftrace_event_id,
                           const uint8_t* start,
                           const uint8_t* end,
                           const ProtoTranslationTable* table,
                           protozero::ProtoZeroMessage* message) {
  const uint8_t* ptr = start;

  // Common headers:
  // TODO(hjd): Convert this to use common fields.
  uint16_t ftrace_event_id_again;
  uint8_t flags;
  uint8_t preempt_count;
  uint32_t pid;
  if (!ReadAndAdvance<uint16_t>(&ptr, end, &ftrace_event_id_again))
    return false;
  if (!ReadAndAdvance<uint8_t>(&ptr, end, &flags))
    return false;
  if (!ReadAndAdvance<uint8_t>(&ptr, end, &preempt_count))
    return false;
  if (!ReadAndAdvance<uint32_t>(&ptr, end, &pid))
    return false;
  message->AppendVarInt<uint32_t>(1, pid);

  PERFETTO_DCHECK(ftrace_event_id == ftrace_event_id_again);

  const Event& info = *table->GetEventById(ftrace_event_id);
  protozero::ProtoZeroMessage* nested =
      message->BeginNestedMessage<protozero::ProtoZeroMessage>(
          info.proto_field_id);

  // TODO(hjd): Replace ReadAndAdvance with single max(offset + size) check.
  // TODO(hjd): Decide read strategy at start time.
  for (const Field& field : info.fields) {
    const uint8_t* p = start + field.ftrace_offset;
    if (field.ftrace_type == kFtraceUint32 &&
        field.proto_field_type == kProtoUint32) {
      uint32_t number;
      if (!ReadAndAdvance<uint32_t>(&p, end, &number))
        return false;
      nested->AppendVarInt<uint32_t>(field.proto_field_id, number);
    } else if (field.ftrace_type == kFtraceUint64 &&
               field.proto_field_type == kProtoUint64) {
      uint64_t number;
      if (!ReadAndAdvance<uint64_t>(&p, end, &number))
        return false;
      nested->AppendVarInt<uint64_t>(field.proto_field_id, number);
    } else if (field.ftrace_type == kFtraceChar16 &&
               field.proto_field_type == kProtoString) {
      // TODO(hjd): Add AppendMaxLength string to protozero.
      char str[16];
      if (!ReadAndAdvance<char[16]>(&p, end, &str))
        return false;
      str[15] = '\0';
      nested->AppendString(field.proto_field_id, str);
    } else if (field.ftrace_type == kFtraceCString &&
               field.proto_field_type == kProtoString) {
      // TODO(hjd): Kernel-dive to check this how size:0 char fields work.
      const uint8_t* str_start = p;
      const uint8_t* str_end = end;
      for (const uint8_t* c = str_start; c < str_end; c++) {
        if (*c != '\0')
          continue;
        nested->AppendBytes(field.proto_field_id,
                            reinterpret_cast<const char*>(str_start),
                            c - str_start);
        break;
      }
    } else {
      PERFETTO_DLOG("Can't read ftrace type '%s' into proto type '%s'",
                    ToString(field.ftrace_type),
                    ToString(field.proto_field_type));
      PERFETTO_CHECK(false);
    }
  }
  // This finalizes |nested| automatically.
  message->Finalize();
  return true;
}

}  // namespace perfetto
