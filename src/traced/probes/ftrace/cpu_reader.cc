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

#include "src/traced/probes/ftrace/cpu_reader.h"

#include <signal.h>

#include <dirent.h>
#include <map>
#include <queue>
#include <string>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/utils.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

#include "perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/ftrace/generic.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

// For further documentation of these constants see the kernel source:
// linux/include/linux/ring_buffer.h
// Some information about the values of these constants are exposed to user
// space at: /sys/kernel/debug/tracing/events/header_event
constexpr uint32_t kTypeDataTypeLengthMax = 28;
constexpr uint32_t kTypePadding = 29;
constexpr uint32_t kTypeTimeExtend = 30;
constexpr uint32_t kTypeTimeStamp = 31;

struct EventHeader {
  uint32_t type_or_length : 5;
  uint32_t time_delta : 27;
};

struct TimeStamp {
  uint64_t tv_nsec;
  uint64_t tv_sec;
};

bool ReadIntoString(const uint8_t* start,
                    const uint8_t* end,
                    uint32_t field_id,
                    protozero::Message* out) {
  for (const uint8_t* c = start; c < end; c++) {
    if (*c != '\0')
      continue;
    out->AppendBytes(field_id, reinterpret_cast<const char*>(start),
                     static_cast<uintptr_t>(c - start));
    return true;
  }
  return false;
}

bool ReadDataLoc(const uint8_t* start,
                 const uint8_t* field_start,
                 const uint8_t* end,
                 const Field& field,
                 protozero::Message* message) {
  PERFETTO_DCHECK(field.ftrace_size == 4);
  // See
  // https://github.com/torvalds/linux/blob/master/include/trace/trace_events.h
  uint32_t data = 0;
  const uint8_t* ptr = field_start;
  if (!CpuReader::ReadAndAdvance(&ptr, end, &data)) {
    PERFETTO_DFATAL("Buffer overflowed.");
    return false;
  }

  const uint16_t offset = data & 0xffff;
  const uint16_t len = (data >> 16) & 0xffff;
  const uint8_t* const string_start = start + offset;
  const uint8_t* const string_end = string_start + len;
  if (string_start <= start || string_end > end) {
    PERFETTO_DFATAL("Buffer overflowed.");
    return false;
  }
  ReadIntoString(string_start, string_end, field.proto_field_id, message);
  return true;
}

bool SetBlocking(int fd, bool is_blocking) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags = (is_blocking) ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  return fcntl(fd, F_SETFL, flags) == 0;
}

}  // namespace

using protos::pbzero::GenericFtraceEvent;

CpuReader::CpuReader(const ProtoTranslationTable* table,
                     size_t cpu,
                     base::ScopedFile trace_fd)
    : table_(table), cpu_(cpu), trace_fd_(std::move(trace_fd)) {
  PERFETTO_CHECK(trace_fd_);
  PERFETTO_CHECK(SetBlocking(*trace_fd_, false));
}

CpuReader::~CpuReader() = default;

size_t CpuReader::ReadCycle(
    uint8_t* parsing_buf,
    size_t parsing_buf_size_pages,
    size_t max_pages,
    const std::set<FtraceDataSource*>& started_data_sources) {
  PERFETTO_DCHECK(max_pages > 0 && parsing_buf_size_pages > 0);
  metatrace::ScopedEvent evt(metatrace::TAG_FTRACE,
                             metatrace::FTRACE_CPU_READ_CYCLE);

  // Work in batches to keep cache locality, and limit memory usage.
  size_t batch_pages = std::min(parsing_buf_size_pages, max_pages);
  size_t total_pages_read = 0;
  for (bool is_first_batch = true;; is_first_batch = false) {
    size_t pages_read = ReadAndProcessBatch(
        parsing_buf, batch_pages, is_first_batch, started_data_sources);

    PERFETTO_DCHECK(pages_read <= batch_pages);
    total_pages_read += pages_read;

    // Check whether we've caught up to the writer, or possibly giving up on
    // this attempt due to some error.
    if (pages_read != batch_pages)
      break;
    // Check if we've hit the limit of work for this cycle.
    if (total_pages_read >= max_pages)
      break;
  }
  PERFETTO_METATRACE_COUNTER(TAG_FTRACE, FTRACE_PAGES_DRAINED,
                             total_pages_read);
  return total_pages_read;
}

// metatrace note: mark the reading phase as FTRACE_CPU_READ_BATCH, but let the
// parsing time be implied (by the difference between the caller's span, and
// this reading span). Makes it easier to estimate the read/parse ratio when
// looking at the trace in the UI.
size_t CpuReader::ReadAndProcessBatch(
    uint8_t* parsing_buf,
    size_t max_pages,
    bool first_batch_in_cycle,
    const std::set<FtraceDataSource*>& started_data_sources) {
  size_t pages_read = 0;
  {
    metatrace::ScopedEvent evt(metatrace::TAG_FTRACE,
                               metatrace::FTRACE_CPU_READ_BATCH);
    for (; pages_read < max_pages;) {
      uint8_t* curr_page = parsing_buf + (pages_read * base::kPageSize);
      ssize_t res =
          PERFETTO_EINTR(read(*trace_fd_, curr_page, base::kPageSize));
      if (res < 0) {
        // Expected errors:
        // EAGAIN: no data (since we're in non-blocking mode).
        // ENONMEM, EBUSY: temporary ftrace failures (they happen).
        if (errno != EAGAIN && errno != ENOMEM && errno != EBUSY)
          PERFETTO_PLOG("Unexpected error on raw ftrace read");
        break;  // stop reading regardless of errno
      }

      // As long as all of our reads are for a single page, the kernel should
      // return exactly a well-formed raw ftrace page (if not in the steady
      // state of reading out fully-written pages, the kernel will construct
      // pages as necessary, copying over events and zero-filling at the end).
      // A sub-page read() is therefore not expected in practice (unless
      // there's a concurrent reader requesting less than a page?). Crash if
      // encountering this situation. Kernel source pointer: see usage of
      // |info->read| within |tracing_buffers_read|.
      // TODO(rsavitski): don't crash, throw away the partial read & pipe
      // through an error signal.
      if (res == 0) {
        // Very rare, but possible. Stop for now, should recover.
        PERFETTO_DLOG("[cpu%zu]: 0-sized read from ftrace pipe.", cpu_);
        break;
      }
      PERFETTO_CHECK(res == static_cast<ssize_t>(base::kPageSize));

      pages_read += 1;

      // Compare the amount of ftrace data read against an empirical threshold
      // to make an educated guess on whether we should read more. To figure
      // out the amount of ftrace data, we need to parse the page header (since
      // the read always returns a page, zero-filled at the end). If we read
      // fewer bytes than the threshold, it means that we caught up with the
      // write pointer and we started consuming ftrace events in real-time.
      // This cannot be just 4096 because it needs to account for
      // fragmentation, i.e. for the fact that the last trace event didn't fit
      // in the current page and hence the current page was terminated
      // prematurely.
      static constexpr size_t kRoughlyAPage = base::kPageSize - 512;
      const uint8_t* scratch_ptr = curr_page;
      base::Optional<PageHeader> hdr =
          ParsePageHeader(&scratch_ptr, table_->page_header_size_len());
      PERFETTO_DCHECK(hdr && hdr->size > 0 && hdr->size <= base::kPageSize);
      if (!hdr.has_value()) {
        PERFETTO_ELOG("[cpu%zu]: can't parse page header", cpu_);
        break;
      }
      // Note that the first read after starting the read cycle being small is
      // normal. It means that we're given the remainder of events from a
      // page that we've partially consumed during the last read of the previous
      // cycle (having caught up to the writer).
      if (hdr->size < kRoughlyAPage &&
          !(first_batch_in_cycle && pages_read == 1)) {
        break;
      }
    }
  }  // end of metatrace::FTRACE_CPU_READ_BATCH

  // Parse the pages and write to the trace for of all relevant data
  // sources.
  for (size_t i = 0; i < pages_read; i++) {
    uint8_t* curr_page = parsing_buf + (i * base::kPageSize);
    for (FtraceDataSource* data_source : started_data_sources) {
      auto packet = data_source->trace_writer()->NewTracePacket();
      auto* bundle = packet->set_ftrace_events();
      auto* metadata = data_source->mutable_metadata();
      auto* filter = data_source->event_filter();

      // Note: The fastpath in proto_trace_parser.cc speculates on the fact
      // that the cpu field is the first field of the proto message. If this
      // changes, change proto_trace_parser.cc accordingly.
      bundle->set_cpu(static_cast<uint32_t>(cpu_));

      size_t evt_size = ParsePage(curr_page, filter, bundle, table_, metadata);
      PERFETTO_DCHECK(evt_size);
      bundle->set_lost_events(metadata->lost_events);
    }
  }
  return pages_read;
}

// A page header consists of:
// * timestamp: 8 bytes
// * commit: 8 bytes on 64 bit, 4 bytes on 32 bit kernels
//
// The kernel reports this at /sys/kernel/debug/tracing/events/header_page.
//
// |commit|'s bottom bits represent the length of the payload following this
// header. The top bits have been repurposed as a bitset of flags pertaining to
// data loss. We look only at the "there has been some data lost" flag
// (RB_MISSED_EVENTS), and ignore the relatively tricky "appended the precise
// lost events count past the end of the valid data, as there was room to do so"
// flag (RB_MISSED_STORED).
//
// static
base::Optional<CpuReader::PageHeader> CpuReader::ParsePageHeader(
    const uint8_t** ptr,
    uint16_t page_header_size_len) {
  // Mask for the data length portion of the |commit| field. Note that the
  // kernel implementation never explicitly defines the boundary (beyond using
  // bits 30 and 31 as flags), but 27 bits are mentioned as sufficient in the
  // original commit message, and is the constant used by trace-cmd.
  constexpr static uint64_t kDataSizeMask = (1ull << 27) - 1;
  // If set, indicates that the relevant cpu has lost events since the last read
  // (clearing the bit internally).
  constexpr static uint64_t kMissedEventsFlag = (1ull << 31);

  const uint8_t* end_of_page = *ptr + base::kPageSize;
  PageHeader page_header;
  if (!CpuReader::ReadAndAdvance<uint64_t>(ptr, end_of_page,
                                           &page_header.timestamp))
    return base::nullopt;

  uint32_t size_and_flags;

  // On little endian, we can just read a uint32_t and reject the rest of the
  // number later.
  if (!CpuReader::ReadAndAdvance<uint32_t>(
          ptr, end_of_page, base::AssumeLittleEndian(&size_and_flags)))
    return base::nullopt;

  page_header.size = size_and_flags & kDataSizeMask;
  page_header.lost_events = bool(size_and_flags & kMissedEventsFlag);
  PERFETTO_DCHECK(page_header.size <= base::kPageSize);

  // Reject rest of the number, if applicable. On 32-bit, size_bytes - 4 will
  // evaluate to 0 and this will be a no-op. On 64-bit, this will advance by 4
  // bytes.
  PERFETTO_DCHECK(page_header_size_len >= 4);
  *ptr += page_header_size_len - 4;

  return base::make_optional(page_header);
}

// A raw ftrace buffer page consists of a header followed by a sequence of
// binary ftrace events. See |ParsePageHeader| for the format of the earlier.
//
// This method is deliberately static so it can be tested independently.
size_t CpuReader::ParsePage(const uint8_t* ptr,
                            const EventFilter* filter,
                            FtraceEventBundle* bundle,
                            const ProtoTranslationTable* table,
                            FtraceMetadata* metadata) {
  const uint8_t* const start_of_page = ptr;
  const uint8_t* const end_of_page = ptr + base::kPageSize;

  auto page_header = ParsePageHeader(&ptr, table->page_header_size_len());
  if (!page_header.has_value())
    return 0;

  // ParsePageHeader advances |ptr| to point past the end of the header.

  metadata->lost_events = static_cast<uint32_t>(page_header->lost_events);
  const uint8_t* const end = ptr + page_header->size;
  if (end > end_of_page)
    return 0;

  uint64_t timestamp = page_header->timestamp;

  while (ptr < end) {
    EventHeader event_header;
    if (!ReadAndAdvance(&ptr, end, &event_header))
      return 0;

    timestamp += event_header.time_delta;

    switch (event_header.type_or_length) {
      case kTypePadding: {
        // Left over page padding or discarded event.
        if (event_header.time_delta == 0) {
          // Not clear what the correct behaviour is in this case.
          PERFETTO_DFATAL("Empty padding event.");
          return 0;
        }
        uint32_t length;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &length))
          return 0;
        ptr += length;
        break;
      }
      case kTypeTimeExtend: {
        // Extend the time delta.
        uint32_t time_delta_ext;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &time_delta_ext))
          return 0;
        // See https://goo.gl/CFBu5x
        timestamp += (static_cast<uint64_t>(time_delta_ext)) << 27;
        break;
      }
      case kTypeTimeStamp: {
        // Sync time stamp with external clock.
        TimeStamp time_stamp;
        if (!ReadAndAdvance<TimeStamp>(&ptr, end, &time_stamp))
          return 0;
        // Not implemented in the kernel, nothing should generate this.
        PERFETTO_DFATAL("Unimplemented in kernel. Should be unreachable.");
        break;
      }
      // Data record:
      default: {
        PERFETTO_CHECK(event_header.type_or_length <= kTypeDataTypeLengthMax);
        // type_or_length is <=28 so it represents the length of a data
        // record. if == 0, this is an extended record and the size of the
        // record is stored in the first uint32_t word in the payload. See
        // Kernel's include/linux/ring_buffer.h
        uint32_t event_size;
        if (event_header.type_or_length == 0) {
          if (!ReadAndAdvance<uint32_t>(&ptr, end, &event_size))
            return 0;
          // Size includes the size field itself.
          if (event_size < 4)
            return 0;
          event_size -= 4;
        } else {
          event_size = 4 * event_header.type_or_length;
        }
        const uint8_t* start = ptr;
        const uint8_t* next = ptr + event_size;

        if (next > end)
          return 0;

        uint16_t ftrace_event_id;
        if (!ReadAndAdvance<uint16_t>(&ptr, end, &ftrace_event_id))
          return 0;
        if (filter->IsEventEnabled(ftrace_event_id)) {
          protos::pbzero::FtraceEvent* event = bundle->add_event();
          event->set_timestamp(timestamp);
          if (!ParseEvent(ftrace_event_id, start, next, table, event, metadata))
            return 0;
        }

        // Jump to next event.
        ptr = next;
      }
    }
  }
  return static_cast<size_t>(ptr - start_of_page);
}

// |start| is the start of the current event.
// |end| is the end of the buffer.
bool CpuReader::ParseEvent(uint16_t ftrace_event_id,
                           const uint8_t* start,
                           const uint8_t* end,
                           const ProtoTranslationTable* table,
                           protozero::Message* message,
                           FtraceMetadata* metadata) {
  PERFETTO_DCHECK(start < end);
  const size_t length = static_cast<size_t>(end - start);

  // TODO(hjd): Rework to work even if the event is unknown.
  const Event& info = *table->GetEventById(ftrace_event_id);

  // TODO(hjd): Test truncated events.
  // If the end of the buffer is before the end of the event give up.
  if (info.size > length) {
    PERFETTO_DFATAL("Buffer overflowed.");
    return false;
  }

  bool success = true;
  for (const Field& field : table->common_fields())
    success &= ParseField(field, start, end, message, metadata);

  protozero::Message* nested =
      message->BeginNestedMessage<protozero::Message>(info.proto_field_id);

  // Parse generic event.
  if (info.proto_field_id == protos::pbzero::FtraceEvent::kGenericFieldNumber) {
    nested->AppendString(GenericFtraceEvent::kEventNameFieldNumber, info.name);
    for (const Field& field : info.fields) {
      auto generic_field = nested->BeginNestedMessage<protozero::Message>(
          GenericFtraceEvent::kFieldFieldNumber);
      // TODO(taylori): Avoid outputting field names every time.
      generic_field->AppendString(GenericFtraceEvent::Field::kNameFieldNumber,
                                  field.ftrace_name);
      success &= ParseField(field, start, end, generic_field, metadata);
    }
  } else {  // Parse all other events.
    for (const Field& field : info.fields) {
      success &= ParseField(field, start, end, nested, metadata);
    }
  }

  if (PERFETTO_UNLIKELY(info.proto_field_id ==
                        protos::pbzero::FtraceEvent::kTaskRenameFieldNumber)) {
    // For task renames, we want to store that the pid was renamed. We use the
    // common pid to reduce code complexity as in all the cases we care about,
    // the common pid is the same as the renamed pid (the pid inside the event).
    PERFETTO_DCHECK(metadata->last_seen_common_pid);
    metadata->AddRenamePid(metadata->last_seen_common_pid);
  }

  // This finalizes |nested| and |proto_field| automatically.
  message->Finalize();
  metadata->FinishEvent();
  return success;
}

// Caller must guarantee that the field fits in the range,
// explicitly: start + field.ftrace_offset + field.ftrace_size <= end
// The only exception is fields with strategy = kCStringToString
// where the total size isn't known up front. In this case ParseField
// will check the string terminates in the bounds and won't read past |end|.
bool CpuReader::ParseField(const Field& field,
                           const uint8_t* start,
                           const uint8_t* end,
                           protozero::Message* message,
                           FtraceMetadata* metadata) {
  PERFETTO_DCHECK(start + field.ftrace_offset + field.ftrace_size <= end);
  const uint8_t* field_start = start + field.ftrace_offset;
  uint32_t field_id = field.proto_field_id;

  switch (field.strategy) {
    case kUint8ToUint32:
    case kUint8ToUint64:
      ReadIntoVarInt<uint8_t>(field_start, field_id, message);
      return true;
    case kUint16ToUint32:
    case kUint16ToUint64:
      ReadIntoVarInt<uint16_t>(field_start, field_id, message);
      return true;
    case kUint32ToUint32:
    case kUint32ToUint64:
      ReadIntoVarInt<uint32_t>(field_start, field_id, message);
      return true;
    case kUint64ToUint64:
      ReadIntoVarInt<uint64_t>(field_start, field_id, message);
      return true;
    case kInt8ToInt32:
    case kInt8ToInt64:
      ReadIntoVarInt<int8_t>(field_start, field_id, message);
      return true;
    case kInt16ToInt32:
    case kInt16ToInt64:
      ReadIntoVarInt<int16_t>(field_start, field_id, message);
      return true;
    case kInt32ToInt32:
    case kInt32ToInt64:
      ReadIntoVarInt<int32_t>(field_start, field_id, message);
      return true;
    case kInt64ToInt64:
      ReadIntoVarInt<int64_t>(field_start, field_id, message);
      return true;
    case kFixedCStringToString:
      // TODO(hjd): Add AppendMaxLength string to protozero.
      return ReadIntoString(field_start, field_start + field.ftrace_size,
                            field_id, message);
    case kCStringToString:
      // TODO(hjd): Kernel-dive to check this how size:0 char fields work.
      return ReadIntoString(field_start, end, field.proto_field_id, message);
    case kStringPtrToString:
      // TODO(hjd): Figure out how to read these.
      return true;
    case kDataLocToString:
      return ReadDataLoc(start, field_start, end, field, message);
    case kBoolToUint32:
    case kBoolToUint64:
      ReadIntoVarInt<uint8_t>(field_start, field_id, message);
      return true;
    case kInode32ToUint64:
      ReadInode<uint32_t>(field_start, field_id, message, metadata);
      return true;
    case kInode64ToUint64:
      ReadInode<uint64_t>(field_start, field_id, message, metadata);
      return true;
    case kPid32ToInt32:
    case kPid32ToInt64:
      ReadPid(field_start, field_id, message, metadata);
      return true;
    case kCommonPid32ToInt32:
    case kCommonPid32ToInt64:
      ReadCommonPid(field_start, field_id, message, metadata);
      return true;
    case kDevId32ToUint64:
      ReadDevId<uint32_t>(field_start, field_id, message, metadata);
      return true;
    case kDevId64ToUint64:
      ReadDevId<uint64_t>(field_start, field_id, message, metadata);
      return true;
  }
  PERFETTO_FATAL("Not reached");  // For gcc
}

}  // namespace perfetto
