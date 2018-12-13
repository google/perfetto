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
#include "perfetto/base/metatrace.h"
#include "perfetto/base/optional.h"
#include "perfetto/base/utils.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/ftrace/ftrace_thread_sync.h"
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

constexpr uint32_t kMainThread = 255;  // for METATRACE

struct PageHeader {
  uint64_t timestamp;
  uint64_t size;
  uint64_t overwrite;
};

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

base::Optional<PageHeader> ParsePageHeader(const uint8_t** ptr,
                                           uint16_t page_header_size_len) {
  const uint8_t* end_of_page = *ptr + base::kPageSize;
  PageHeader page_header;
  if (!CpuReader::ReadAndAdvance<uint64_t>(ptr, end_of_page,
                                           &page_header.timestamp))
    return base::nullopt;

  uint32_t overwrite_and_size;

  // On little endian, we can just read a uint32_t and reject the rest of the
  // number later.
  if (!CpuReader::ReadAndAdvance<uint32_t>(
          ptr, end_of_page, base::AssumeLittleEndian(&overwrite_and_size)))
    return base::nullopt;

  page_header.size = (overwrite_and_size & 0x000000000000ffffull) >> 0;
  page_header.overwrite = (overwrite_and_size & 0x00000000ff000000ull) >> 24;
  PERFETTO_DCHECK(page_header.size <= base::kPageSize);

  // Reject rest of the number, if applicable. On 32-bit, size_bytes - 4 will
  // evaluate to 0 and this will be a no-op. On 64-bit, this will advance by 4
  // bytes.
  PERFETTO_DCHECK(page_header_size_len >= 4);
  *ptr += page_header_size_len - 4;

  return base::make_optional(page_header);
}

}  // namespace

using protos::pbzero::GenericFtraceEvent;

CpuReader::CpuReader(const ProtoTranslationTable* table,
                     FtraceThreadSync* thread_sync,
                     size_t cpu,
                     int generation,
                     base::ScopedFile fd)
    : table_(table),
      thread_sync_(thread_sync),
      cpu_(cpu),
      trace_fd_(std::move(fd)) {
  // Make reads from the raw pipe blocking so that splice() can sleep.
  PERFETTO_CHECK(trace_fd_);
  PERFETTO_CHECK(SetBlocking(*trace_fd_, true));

  // We need a non-default SIGPIPE handler to make it so that the blocking
  // splice() is woken up when the ~CpuReader() dtor destroys the pipes.
  // Just masking out the signal would cause an implicit syscall restart and
  // hence make the join() in the dtor unreliable.
  struct sigaction current_act = {};
  PERFETTO_CHECK(sigaction(SIGPIPE, nullptr, &current_act) == 0);
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  if (current_act.sa_handler == SIG_DFL || current_act.sa_handler == SIG_IGN) {
    struct sigaction act = {};
    act.sa_sigaction = [](int, siginfo_t*, void*) {};
    PERFETTO_CHECK(sigaction(SIGPIPE, &act, nullptr) == 0);
  }
#pragma GCC diagnostic pop

  worker_thread_ = std::thread(std::bind(&RunWorkerThread, cpu_, generation,
                                         *trace_fd_, &pool_, thread_sync_,
                                         table->page_header_size_len()));
}

CpuReader::~CpuReader() {
// FtraceController (who owns this) is supposed to issue a kStop notification
// to the thread sync object before destroying the CpuReader.
#if PERFETTO_DCHECK_IS_ON()
  {
    std::lock_guard<std::mutex> lock(thread_sync_->mutex);
    PERFETTO_DCHECK(thread_sync_->cmd == FtraceThreadSync::kQuit);
  }
#endif

  // The kernel's splice implementation for the trace pipe doesn't generate a
  // SIGPIPE if the output pipe is closed (b/73807072). Instead, the call to
  // close() on the pipe hangs forever. To work around this, we first close the
  // trace fd (which prevents another splice from starting), raise SIGPIPE and
  // wait for the worker to exit (i.e., to guarantee no splice is in progress)
  // and only then close the staging pipe.
  trace_fd_.reset();
  InterruptWorkerThreadWithSignal();
  worker_thread_.join();
}

void CpuReader::InterruptWorkerThreadWithSignal() {
  pthread_kill(worker_thread_.native_handle(), SIGPIPE);
}

// The worker thread reads data from the ftrace trace_pipe_raw and moves it to
// the page |pool| allowing the main thread to read and decode that.
// See //docs/ftrace.md for the design of the ftrace worker scheduler.
// static
void CpuReader::RunWorkerThread(size_t cpu,
                                int generation,
                                int trace_fd,
                                PagePool* pool,
                                FtraceThreadSync* thread_sync,
                                uint16_t header_size_len) {
// Before attempting any changes to this function, think twice. The kernel
// ftrace pipe code is full of caveats and bugs. This code carefully works
// around those bugs. See b/120188810 and b/119805587 for the full narrative.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  char thread_name[16];
  snprintf(thread_name, sizeof(thread_name), "traced_probes%zu", cpu);
  pthread_setname_np(pthread_self(), thread_name);

  // When using splice() the target fd needs to be an actual pipe. This pipe is
  // used only within this thread and is mainly for synchronization purposes.
  // A blocking splice() is the only way to block and wait for a new page of
  // ftrace data.
  base::Pipe sync_pipe = base::Pipe::Create(base::Pipe::kBothNonBlock);

  enum ReadMode { kRead, kSplice };
  enum Block { kBlock, kNonBlock };
  constexpr auto kPageSize = base::kPageSize;

  // This lambda function reads the ftrace raw pipe using either read() or
  // splice(), either in blocking or non-blocking mode.
  // Returns the number of ftrace bytes read, or -1 in case of failure.
  auto read_ftrace_pipe = [&sync_pipe, trace_fd, pool, cpu, header_size_len](
                              ReadMode mode, Block block) -> int {
    static const char* const kModesStr[] = {"read-nonblock", "read-block",
                                            "splice-nonblock", "splice-block"};
    const char* mode_str = kModesStr[(mode == kSplice) * 2 + (block == kBlock)];
    PERFETTO_METATRACE(mode_str, cpu);
    uint8_t* pool_page = pool->BeginWrite();
    PERFETTO_DCHECK(pool_page);

    ssize_t res;
    int err = 0;
    if (mode == kSplice) {
      uint32_t flg = SPLICE_F_MOVE | ((block == kNonBlock) * SPLICE_F_NONBLOCK);
      res = splice(trace_fd, nullptr, *sync_pipe.wr, nullptr, kPageSize, flg);
      err = errno;
      if (res > 0) {
        // If the splice() succeeded, read back from the other end of our own
        // pipe and copy the data into the pool.
        ssize_t rdres = read(*sync_pipe.rd, pool_page, kPageSize);
        PERFETTO_DCHECK(rdres = res);
      }
    } else {
      if (block == kNonBlock)
        SetBlocking(trace_fd, false);
      res = read(trace_fd, pool_page, kPageSize);
      err = errno;
      if (res > 0) {
        // Need to copy the ptr, ParsePageHeader() advances the passed ptr arg.
        const uint8_t* ptr = pool_page;

        // The caller of this function wants to have a sufficient approximation
        // of how many bytes of ftrace data have been read. Unfortunately the
        // return value of read() is a lie. The problem is that the ftrace
        // read() implementation, for good reasons, always reconstructs a whole
        // ftrace page, copying the events over and zero-filling at the end.
        // This is nice, because we always get a valid ftrace header, but also
        // causes read to always returns 4096. The only way to have a good
        // indication of how many bytes of ftrace data have been read is to
        // parse the ftrace header.
        // Note: |header_size_len| is *not* an indication on how many bytes are
        // available form |ptr|. It's just an independent piece of information
        // that needs to be passed to ParsePageHeader() (a static function) in
        // order to work.
        base::Optional<PageHeader> hdr = ParsePageHeader(&ptr, header_size_len);
        PERFETTO_DCHECK(hdr && hdr->size > 0 && hdr->size <= base::kPageSize);
        res = hdr.has_value() ? static_cast<int>(hdr->size) : -1;
      }
      if (block == kNonBlock)
        SetBlocking(trace_fd, true);
    }

    if (res > 0) {
      // splice() should return full pages, read can return < a page.
      PERFETTO_DCHECK(res == base::kPageSize || mode == kRead);
      pool->EndWrite();
      return static_cast<int>(res);
    }

    // It is fine to leave the BeginWrite() unpaired in the error case.

    if (res && err != EAGAIN && err != ENOMEM && err != EBUSY && err != EINTR &&
        err != EBADF) {
      // EAGAIN: no data when in non-blocking mode.
      // ENONMEM, EBUSY: temporary ftrace failures (they happen).
      // EINTR: signal interruption, likely from main thread to issue a new cmd.
      // EBADF: the main thread has closed the fd (happens during dtor).
      PERFETTO_PLOG("Unexpected %s() err", mode == kRead ? "read" : "splice");
    }
    return -1;
  };

  uint64_t last_cmd_id = 0;
  ReadMode cur_mode = kSplice;
  for (bool run_loop = true; run_loop;) {
    FtraceThreadSync::Cmd cmd;
    // Wait for a new command from the main thread issued by FtraceController.
    // The FtraceController issues also a signal() after every new command. This
    // is not necessary for the condition variable itself, but it's necessary to
    // unblock us if we are in a blocking read() or splice().
    // Commands are tagged with an ID, every new command has a new |cmd_id|, so
    // we can distinguish spurious wakeups from actual cmd requests.
    {
      PERFETTO_METATRACE("wait cmd", cpu);
      std::unique_lock<std::mutex> lock(thread_sync->mutex);
      while (thread_sync->cmd_id == last_cmd_id)
        thread_sync->cond.wait(lock);
      cmd = thread_sync->cmd;
      last_cmd_id = thread_sync->cmd_id;
    }

    // An empirical threshold (bytes read/spliced from the raw pipe) to make an
    // educated guess on whether we should read/splice more. If we read fewer
    // bytes it means that we caught up with the write pointer and we started
    // consuming ftrace events in real-time. This cannot be just 4096 because
    // it needs to account for fragmentation, i.e. for the fact that the last
    // trace event didn't fit in the current page and hence the current page
    // was terminated prematurely.
    constexpr int kRoughlyAPage = 4096 - 512;

    switch (cmd) {
      case FtraceThreadSync::kQuit:
        run_loop = false;
        break;

      case FtraceThreadSync::kRun: {
        PERFETTO_METATRACE(cur_mode == kRead ? "read" : "splice", cpu);

        // Do a blocking read/splice. This can fail for a variety of reasons:
        // - FtraceController interrupts us with a signal for a new cmd
        //   (e.g. it wants us to quit or do a flush).
        // - A temporary read/splice() failure occurred (it has been observed
        //   to happen if the system is under high load).
        // In all these cases the most useful thing we can do is skip the
        // current cycle and try again later.
        if (read_ftrace_pipe(cur_mode, kBlock) <= 0)
          break;  // Wait for next command.

        // If we are in read mode (because of a previous flush) check if the
        // in-kernel read cursor is page-aligned again. If a non-blocking splice
        // succeeds, it means that we can safely switch back to splice mode
        // (See b/120188810).
        if (cur_mode == kRead && read_ftrace_pipe(kSplice, kNonBlock) > 0)
          cur_mode = kSplice;

        // Do as many non-blocking read/splice as we can.
        while (read_ftrace_pipe(cur_mode, kNonBlock) > kRoughlyAPage) {
        }
        pool->CommitWrittenPages();
        FtraceController::OnCpuReaderRead(cpu, generation, thread_sync);
        break;
      }

      case FtraceThreadSync::kFlush: {
        PERFETTO_METATRACE("flush", cpu);
        cur_mode = kRead;
        while (read_ftrace_pipe(cur_mode, kNonBlock) > kRoughlyAPage) {
        }
        pool->CommitWrittenPages();
        FtraceController::OnCpuReaderFlush(cpu, generation, thread_sync);
        break;
      }
    }  // switch(cmd)
  }    // for(run_loop)
  PERFETTO_DPLOG("Terminating CPUReader thread for CPU %zd.", cpu);
#else
  base::ignore_result(cpu);
  base::ignore_result(generation);
  base::ignore_result(trace_fd);
  base::ignore_result(pool);
  base::ignore_result(thread_sync);
  base::ignore_result(header_size_len);
  PERFETTO_ELOG("Supported only on Linux/Android");
#endif
}

// Invoked on the main thread by FtraceController, |drain_rate_ms| after the
// first CPU wakes up from the blocking read()/splice().
void CpuReader::Drain(const std::set<FtraceDataSource*>& data_sources) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_METATRACE("Drain(" + std::to_string(cpu_) + ")", kMainThread);

  auto page_blocks = pool_.BeginRead();
  for (const auto& page_block : page_blocks) {
    for (size_t i = 0; i < page_block.size(); i++) {
      const uint8_t* page = page_block.At(i);

      for (FtraceDataSource* data_source : data_sources) {
        auto packet = data_source->trace_writer()->NewTracePacket();
        auto* bundle = packet->set_ftrace_events();
        auto* metadata = data_source->mutable_metadata();
        auto* filter = data_source->event_filter();

        // Note: The fastpath in proto_trace_parser.cc speculates on the fact
        // that the cpu field is the first field of the proto message. If this
        // changes, change proto_trace_parser.cc accordingly.
        bundle->set_cpu(static_cast<uint32_t>(cpu_));

        size_t evt_size = ParsePage(page, filter, bundle, table_, metadata);
        PERFETTO_DCHECK(evt_size);
        bundle->set_overwrite_count(metadata->overwrite_count);
      }
    }
  }
  pool_.EndRead(std::move(page_blocks));
}

// The structure of a raw trace buffer page is as follows:
// First a page header:
//   8 bytes of timestamp
//   8 bytes of page length TODO(hjd): other fields also defined here?
// // TODO(hjd): Document rest of format.
// Some information about the layout of the page header is available in user
// space at: /sys/kernel/debug/tracing/events/header_event
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

  metadata->overwrite_count = static_cast<uint32_t>(page_header->overwrite);
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
