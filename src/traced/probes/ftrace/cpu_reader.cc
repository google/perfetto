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

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

#include <algorithm>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "src/kallsyms/kernel_symbol_map.h"
#include "src/kallsyms/lazy_kernel_symbolizer.h"
#include "src/traced/probes/ftrace/cpu_stats_parser.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/ftrace/ftrace_print_filter.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

// If the compact_sched buffer accumulates more unique strings, the reader will
// flush it to reset the interning state (and make it cheap again).
// This is not an exact cap, since we check only at tracing page boundaries.
constexpr size_t kCompactSchedInternerThreshold = 64;

// For further documentation of these constants see the kernel source:
//   linux/include/linux/ring_buffer.h
// Some of this is also available to userspace at runtime via:
//   /sys/kernel/tracing/events/header_event
constexpr uint32_t kTypeDataTypeLengthMax = 28;
constexpr uint32_t kTypePadding = 29;
constexpr uint32_t kTypeTimeExtend = 30;
constexpr uint32_t kTypeTimeStamp = 31;

struct EventHeader {
  // bottom 5 bits
  uint32_t type_or_length : 5;
  // top 27 bits
  uint32_t time_delta : 27;
};

// Reads a string from `start` until the first '\0' byte or until fixed_len
// characters have been read. Appends it to `*out` as field `field_id`.
void ReadIntoString(const uint8_t* start,
                    size_t fixed_len,
                    uint32_t field_id,
                    protozero::Message* out) {
  size_t len = strnlen(reinterpret_cast<const char*>(start), fixed_len);
  out->AppendBytes(field_id, reinterpret_cast<const char*>(start), len);
}

bool ReadDataLoc(const uint8_t* start,
                 const uint8_t* field_start,
                 const uint8_t* end,
                 const Field& field,
                 protozero::Message* message) {
  PERFETTO_DCHECK(field.ftrace_size == 4);
  // See kernel header include/trace/trace_events.h
  uint32_t data = 0;
  const uint8_t* ptr = field_start;
  if (!CpuReader::ReadAndAdvance(&ptr, end, &data)) {
    PERFETTO_DFATAL("couldn't read __data_loc value");
    return false;
  }

  const uint16_t offset = data & 0xffff;
  const uint16_t len = (data >> 16) & 0xffff;
  const uint8_t* const string_start = start + offset;

  if (PERFETTO_UNLIKELY(len == 0))
    return true;
  if (PERFETTO_UNLIKELY(string_start < start || string_start + len > end)) {
    PERFETTO_DFATAL("__data_loc points at invalid location");
    return false;
  }
  ReadIntoString(string_start, len, field.proto_field_id, message);
  return true;
}

template <typename T>
T ReadValue(const uint8_t* ptr) {
  T t;
  memcpy(&t, reinterpret_cast<const void*>(ptr), sizeof(T));
  return t;
}

// Reads a signed ftrace value as an int64_t, sign extending if necessary.
static int64_t ReadSignedFtraceValue(const uint8_t* ptr,
                                     FtraceFieldType ftrace_type) {
  if (ftrace_type == kFtraceInt32) {
    int32_t value;
    memcpy(&value, reinterpret_cast<const void*>(ptr), sizeof(value));
    return int64_t(value);
  }
  if (ftrace_type == kFtraceInt64) {
    int64_t value;
    memcpy(&value, reinterpret_cast<const void*>(ptr), sizeof(value));
    return value;
  }
  PERFETTO_FATAL("unexpected ftrace type");
}

bool SetBlocking(int fd, bool is_blocking) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags = (is_blocking) ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  return fcntl(fd, F_SETFL, flags) == 0;
}

bool ZeroPaddedPageTail(const uint8_t* start, const uint8_t* end) {
  for (auto p = start; p < end; p++) {
    if (*p != 0)
      return false;
  }
  return true;
}

void LogInvalidPage(const void* start, size_t size) {
  PERFETTO_ELOG("Invalid ftrace page");
  std::string hexdump = base::HexDump(start, size);
  // Only a single line per log message, because log message size might be
  // limited.
  for (base::StringSplitter ss(std::move(hexdump), '\n'); ss.Next();) {
    PERFETTO_ELOG("%s", ss.cur_token());
  }
}

}  // namespace

using protos::pbzero::GenericFtraceEvent;

CpuReader::CpuReader(size_t cpu,
                     const ProtoTranslationTable* table,
                     LazyKernelSymbolizer* symbolizer,
                     const FtraceClockSnapshot* ftrace_clock_snapshot,
                     base::ScopedFile trace_fd)
    : cpu_(cpu),
      table_(table),
      symbolizer_(symbolizer),
      ftrace_clock_snapshot_(ftrace_clock_snapshot),
      trace_fd_(std::move(trace_fd)) {
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
  size_t total_pages_read = 0;
  for (bool is_first_batch = true;; is_first_batch = false) {
    size_t batch_pages =
        std::min(parsing_buf_size_pages, max_pages - total_pages_read);
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
        // ENODEV: the cpu is offline (b/145583318).
        if (errno != EAGAIN && errno != ENOMEM && errno != EBUSY &&
            errno != ENODEV) {
          PERFETTO_PLOG("Unexpected error on raw ftrace read");
        }
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

  // Parse the pages and write to the trace for all relevant data
  // sources.
  if (pages_read == 0)
    return pages_read;

  for (FtraceDataSource* data_source : started_data_sources) {
    size_t pages_parsed_ok = ProcessPagesForDataSource(
        data_source->trace_writer(), data_source->mutable_metadata(), cpu_,
        data_source->parsing_config(), parsing_buf, pages_read, table_,
        symbolizer_, ftrace_clock_snapshot_, ftrace_clock_);
    // If this happens, it means that we did not know how to parse the kernel
    // binary format. This is a bug in either perfetto or the kernel, and must
    // be investigated. Hence we abort instead of recording a bit in the ftrace
    // stats proto, which is easier to overlook.
    if (pages_parsed_ok != pages_read) {
      const size_t first_bad_page_idx = pages_parsed_ok;
      const uint8_t* curr_page =
          parsing_buf + (first_bad_page_idx * base::kPageSize);
      LogInvalidPage(curr_page, base::kPageSize);
      PERFETTO_FATAL("Failed to parse ftrace page");
    }
  }

  return pages_read;
}

// static
size_t CpuReader::ProcessPagesForDataSource(
    TraceWriter* trace_writer,
    FtraceMetadata* metadata,
    size_t cpu,
    const FtraceDataSourceConfig* ds_config,
    const uint8_t* parsing_buf,
    const size_t pages_read,
    const ProtoTranslationTable* table,
    LazyKernelSymbolizer* symbolizer,
    const FtraceClockSnapshot* ftrace_clock_snapshot,
    protos::pbzero::FtraceClock ftrace_clock) {
  // Allocate the buffer for compact scheduler events (which will be unused if
  // the compact option isn't enabled).
  CompactSchedBuffer compact_sched;
  bool compact_sched_enabled = ds_config->compact_sched.enabled;

  TraceWriter::TracePacketHandle packet;
  protos::pbzero::FtraceEventBundle* bundle = nullptr;

  // This function is called after the contents of a FtraceBundle are written.
  auto finalize_cur_packet = [&] {
    PERFETTO_DCHECK(packet);
    if (compact_sched_enabled)
      compact_sched.WriteAndReset(bundle);

    bundle->Finalize();
    bundle = nullptr;

    // Write the kernel symbol index (mangled address) -> name table.
    // |metadata| is shared across all cpus, is distinct per |data_source| (i.e.
    // tracing session) and is cleared after each FtraceController::ReadTick().
    if (ds_config->symbolize_ksyms) {
      // Symbol indexes are assigned mononically as |kernel_addrs.size()|,
      // starting from index 1 (no symbol has index 0). Here we remember the
      // size() (which is also == the highest value in |kernel_addrs|) at the
      // beginning and only write newer indexes bigger than that.
      uint32_t max_index_at_start = metadata->last_kernel_addr_index_written;
      PERFETTO_DCHECK(max_index_at_start <= metadata->kernel_addrs.size());
      protos::pbzero::InternedData* interned_data = nullptr;
      auto* ksyms_map = symbolizer->GetOrCreateKernelSymbolMap();
      bool wrote_at_least_one_symbol = false;
      for (const FtraceMetadata::KernelAddr& kaddr : metadata->kernel_addrs) {
        if (kaddr.index <= max_index_at_start)
          continue;
        std::string sym_name = ksyms_map->Lookup(kaddr.addr);
        if (sym_name.empty()) {
          // Lookup failed. This can genuinely happen in many occasions. E.g.,
          // workqueue_execute_start has two pointers: one is a pointer to a
          // function (which we expect to be symbolized), the other (|work|) is
          // a pointer to a heap struct, which is unsymbolizable, even when
          // using the textual ftrace endpoint.
          continue;
        }

        if (!interned_data) {
          // If this is the very first write, clear the start of the sequence
          // so the trace processor knows that all previous indexes can be
          // discarded and that the mapping is restarting.
          // In most cases this occurs with cpu==0. But if cpu0 is idle, this
          // will happen with the first CPU that has any ftrace data.
          if (max_index_at_start == 0) {
            packet->set_sequence_flags(
                protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED);
          }
          interned_data = packet->set_interned_data();
        }
        auto* interned_sym = interned_data->add_kernel_symbols();
        interned_sym->set_iid(kaddr.index);
        interned_sym->set_str(sym_name);
        wrote_at_least_one_symbol = true;
      }

      auto max_it_at_end = static_cast<uint32_t>(metadata->kernel_addrs.size());

      // Rationale for the if (wrote_at_least_one_symbol) check: in rare cases,
      // all symbols seen in a ProcessPagesForDataSource() call can fail the
      // ksyms_map->Lookup(). If that happens we don't want to bump the
      // last_kernel_addr_index_written watermark, as that would cause the next
      // call to NOT emit the SEQ_INCREMENTAL_STATE_CLEARED.
      if (wrote_at_least_one_symbol)
        metadata->last_kernel_addr_index_written = max_it_at_end;
    }

    packet->Finalize();
  };  // finalize_cur_packet().

  auto start_new_packet = [&](bool lost_events) {
    if (packet)
      finalize_cur_packet();
    packet = trace_writer->NewTracePacket();
    bundle = packet->set_ftrace_events();
    if (ftrace_clock) {
      bundle->set_ftrace_clock(ftrace_clock);

      if (ftrace_clock_snapshot && ftrace_clock_snapshot->ftrace_clock_ts) {
        bundle->set_ftrace_timestamp(ftrace_clock_snapshot->ftrace_clock_ts);
        bundle->set_boot_timestamp(ftrace_clock_snapshot->boot_clock_ts);
      }
    }

    // Note: The fastpath in proto_trace_parser.cc speculates on the fact
    // that the cpu field is the first field of the proto message. If this
    // changes, change proto_trace_parser.cc accordingly.
    bundle->set_cpu(static_cast<uint32_t>(cpu));
    if (lost_events)
      bundle->set_lost_events(true);
  };

  start_new_packet(/*lost_events=*/false);
  size_t pages_parsed = 0;
  for (; pages_parsed < pages_read; pages_parsed++) {
    const uint8_t* curr_page = parsing_buf + (pages_parsed * base::kPageSize);
    const uint8_t* curr_page_end = curr_page + base::kPageSize;
    const uint8_t* parse_pos = curr_page;
    base::Optional<PageHeader> page_header =
        ParsePageHeader(&parse_pos, table->page_header_size_len());

    if (!page_header.has_value() || page_header->size == 0 ||
        parse_pos >= curr_page_end ||
        parse_pos + page_header->size > curr_page_end) {
      break;
    }

    // Start a new bundle if either:
    // * The page we're about to read indicates that there was a kernel ring
    //   buffer overrun since our last read from that per-cpu buffer. We have
    //   a single |lost_events| field per bundle, so start a new packet.
    // * The compact_sched buffer is holding more unique interned strings than
    //   a threshold. We need to flush the compact buffer to make the
    //   interning lookups cheap again.
    bool interner_past_threshold =
        compact_sched_enabled &&
        compact_sched.interner().interned_comms_size() >
            kCompactSchedInternerThreshold;

    if (page_header->lost_events || interner_past_threshold)
      start_new_packet(page_header->lost_events);

    size_t evt_size =
        ParsePagePayload(parse_pos, &page_header.value(), table, ds_config,
                         &compact_sched, bundle, metadata);

    if (evt_size != page_header->size) {
      break;
    }
  }
  finalize_cur_packet();

  return pages_parsed;
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
size_t CpuReader::ParsePagePayload(const uint8_t* start_of_payload,
                                   const PageHeader* page_header,
                                   const ProtoTranslationTable* table,
                                   const FtraceDataSourceConfig* ds_config,
                                   CompactSchedBuffer* compact_sched_buffer,
                                   FtraceEventBundle* bundle,
                                   FtraceMetadata* metadata) {
  const uint8_t* ptr = start_of_payload;
  const uint8_t* const end = ptr + page_header->size;

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
        uint32_t length = 0;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &length))
          return 0;
        // length includes itself (4 bytes)
        if (length < 4)
          return 0;
        ptr += length - 4;
        break;
      }
      case kTypeTimeExtend: {
        // Extend the time delta.
        uint32_t time_delta_ext = 0;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &time_delta_ext))
          return 0;
        timestamp += (static_cast<uint64_t>(time_delta_ext)) << 27;
        break;
      }
      case kTypeTimeStamp: {
        // Absolute timestamp. This was historically partially implemented, but
        // not written. Kernels 4.17+ reimplemented this record, changing its
        // size in the process. We assume the newer layout. Parsed the same as
        // kTypeTimeExtend, except that the timestamp is interpreted as an
        // absolute, instead of a delta on top of the previous state.
        uint32_t time_delta_ext = 0;
        if (!ReadAndAdvance<uint32_t>(&ptr, end, &time_delta_ext))
          return 0;
        timestamp = event_header.time_delta +
                    (static_cast<uint64_t>(time_delta_ext) << 27);
        break;
      }
      // Data record:
      default: {
        PERFETTO_CHECK(event_header.type_or_length <= kTypeDataTypeLengthMax);
        // type_or_length is <=28 so it represents the length of a data
        // record. if == 0, this is an extended record and the size of the
        // record is stored in the first uint32_t word in the payload. See
        // Kernel's include/linux/ring_buffer.h
        uint32_t event_size = 0;
        if (event_header.type_or_length == 0) {
          if (!ReadAndAdvance<uint32_t>(&ptr, end, &event_size)) {
            return 0;
          }
          // Size includes the size field itself. Special case for invalid
          // tracing pages seen on select Android 4.19 kernels: the page header
          // says there's still valid data, but the rest of the page is full of
          // zeroes (which would not decode to a valid event). We pretend that
          // such pages have been fully parsed. b/204564312.
          if (PERFETTO_UNLIKELY(event_size == 0 &&
                                event_header.time_delta == 0 &&
                                ZeroPaddedPageTail(ptr, end))) {
            return static_cast<size_t>(page_header->size);
          } else if (PERFETTO_UNLIKELY(event_size < 4)) {
            return 0;
          }
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

        if (ds_config->event_filter.IsEventEnabled(ftrace_event_id)) {
          // Special-cased handling of some scheduler events when compact format
          // is enabled.
          bool compact_sched_enabled = ds_config->compact_sched.enabled;
          const CompactSchedSwitchFormat& sched_switch_format =
              table->compact_sched_format().sched_switch;
          const CompactSchedWakingFormat& sched_waking_format =
              table->compact_sched_format().sched_waking;

          bool ftrace_print_filter_enabled =
              ds_config->print_filter.has_value();

          // compact sched_switch
          if (compact_sched_enabled &&
              ftrace_event_id == sched_switch_format.event_id) {
            if (event_size < sched_switch_format.size)
              return 0;

            ParseSchedSwitchCompact(start, timestamp, &sched_switch_format,
                                    compact_sched_buffer, metadata);

            // compact sched_waking
          } else if (compact_sched_enabled &&
                     ftrace_event_id == sched_waking_format.event_id) {
            if (event_size < sched_waking_format.size)
              return 0;

            ParseSchedWakingCompact(start, timestamp, &sched_waking_format,
                                    compact_sched_buffer, metadata);

          } else if (ftrace_print_filter_enabled &&
                     ftrace_event_id == ds_config->print_filter->event_id()) {
            if (ds_config->print_filter->IsEventInteresting(start, next)) {
              protos::pbzero::FtraceEvent* event = bundle->add_event();
              event->set_timestamp(timestamp);
              if (!ParseEvent(ftrace_event_id, start, next, table, ds_config,
                              event, metadata))
                return 0;
            }
          } else {
            // Common case: parse all other types of enabled events.
            protos::pbzero::FtraceEvent* event = bundle->add_event();
            event->set_timestamp(timestamp);
            if (!ParseEvent(ftrace_event_id, start, next, table, ds_config,
                            event, metadata))
              return 0;
          }
        }

        // Jump to next event.
        ptr = next;
      }
    }
  }
  return static_cast<size_t>(ptr - start_of_payload);
}

// |start| is the start of the current event.
// |end| is the end of the buffer.
bool CpuReader::ParseEvent(uint16_t ftrace_event_id,
                           const uint8_t* start,
                           const uint8_t* end,
                           const ProtoTranslationTable* table,
                           const FtraceDataSourceConfig* ds_config,
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
    success &= ParseField(field, start, end, table, message, metadata);

  protozero::Message* nested =
      message->BeginNestedMessage<protozero::Message>(info.proto_field_id);

  // Parse generic event.
  if (PERFETTO_UNLIKELY(info.proto_field_id ==
                        protos::pbzero::FtraceEvent::kGenericFieldNumber)) {
    nested->AppendString(GenericFtraceEvent::kEventNameFieldNumber, info.name);
    for (const Field& field : info.fields) {
      auto generic_field = nested->BeginNestedMessage<protozero::Message>(
          GenericFtraceEvent::kFieldFieldNumber);
      generic_field->AppendString(GenericFtraceEvent::Field::kNameFieldNumber,
                                  field.ftrace_name);
      success &= ParseField(field, start, end, table, generic_field, metadata);
    }
  } else if (PERFETTO_UNLIKELY(
                 info.proto_field_id ==
                 protos::pbzero::FtraceEvent::kSysEnterFieldNumber)) {
    success &= ParseSysEnter(info, start, end, nested, metadata);
  } else if (PERFETTO_UNLIKELY(
                 info.proto_field_id ==
                 protos::pbzero::FtraceEvent::kSysExitFieldNumber)) {
    success &= ParseSysExit(info, start, end, ds_config, nested, metadata);
  } else {  // Parse all other events.
    for (const Field& field : info.fields) {
      success &= ParseField(field, start, end, table, nested, metadata);
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
                           const ProtoTranslationTable* table,
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
      // TODO(hjd): Kernel-dive to check this how size:0 char fields work.
      ReadIntoString(field_start, field.ftrace_size, field_id, message);
      return true;
    case kCStringToString:
      // TODO(hjd): Kernel-dive to check this how size:0 char fields work.
      ReadIntoString(field_start, static_cast<size_t>(end - field_start),
                     field_id, message);
      return true;
    case kStringPtrToString: {
      uint64_t n = 0;
      // The ftrace field may be 8 or 4 bytes and we need to copy it into the
      // bottom of n. In the unlikely case where the field is >8 bytes we
      // should avoid making things worse by corrupting the stack but we
      // don't need to handle it correctly.
      size_t size = std::min<size_t>(field.ftrace_size, sizeof(n));
      memcpy(base::AssumeLittleEndian(&n),
             reinterpret_cast<const void*>(field_start), size);
      // Look up the adddress in the printk format map and write it into the
      // proto.
      base::StringView name = table->LookupTraceString(n);
      message->AppendBytes(field_id, name.begin(), name.size());
      return true;
    }
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
    case kFtraceSymAddr64ToUint64:
      ReadSymbolAddr<uint64_t>(field_start, field_id, message, metadata);
      return true;
    case kInvalidTranslationStrategy:
      break;
  }
  PERFETTO_FATAL("Unexpected translation strategy");
}

bool CpuReader::ParseSysEnter(const Event& info,
                              const uint8_t* start,
                              const uint8_t* end,
                              protozero::Message* message,
                              FtraceMetadata* /* metadata */) {
  if (info.fields.size() != 2) {
    PERFETTO_DLOG("Unexpected number of fields for sys_enter");
    return false;
  }
  const auto& id_field = info.fields[0];
  const auto& args_field = info.fields[1];
  if (start + id_field.ftrace_size + args_field.ftrace_size > end) {
    return false;
  }
  // field:long id;
  if (id_field.ftrace_type != kFtraceInt32 &&
      id_field.ftrace_type != kFtraceInt64) {
    return false;
  }
  const int64_t syscall_id = ReadSignedFtraceValue(
      start + id_field.ftrace_offset, id_field.ftrace_type);
  message->AppendVarInt(id_field.proto_field_id, syscall_id);
  // field:unsigned long args[6];
  // proto_translation_table will only allow exactly 6-element array, so we can
  // make the same hard assumption here.
  constexpr uint16_t arg_count = 6;
  size_t element_size = 0;
  if (args_field.ftrace_type == kFtraceUint32) {
    element_size = 4u;
  } else if (args_field.ftrace_type == kFtraceUint64) {
    element_size = 8u;
  } else {
    return false;
  }
  for (uint16_t i = 0; i < arg_count; ++i) {
    const uint8_t* element_ptr =
        start + args_field.ftrace_offset + i * element_size;
    uint64_t arg_value = 0;
    if (element_size == 8) {
      arg_value = ReadValue<uint64_t>(element_ptr);
    } else {
      arg_value = ReadValue<uint32_t>(element_ptr);
    }
    message->AppendVarInt(args_field.proto_field_id, arg_value);
  }
  return true;
}

bool CpuReader::ParseSysExit(const Event& info,
                             const uint8_t* start,
                             const uint8_t* end,
                             const FtraceDataSourceConfig* ds_config,
                             protozero::Message* message,
                             FtraceMetadata* metadata) {
  if (info.fields.size() != 2) {
    PERFETTO_DLOG("Unexpected number of fields for sys_exit");
    return false;
  }
  const auto& id_field = info.fields[0];
  const auto& ret_field = info.fields[1];
  if (start + id_field.ftrace_size + ret_field.ftrace_size > end) {
    return false;
  }
  //    field:long id;
  if (id_field.ftrace_type != kFtraceInt32 &&
      id_field.ftrace_type != kFtraceInt64) {
    return false;
  }
  const int64_t syscall_id = ReadSignedFtraceValue(
      start + id_field.ftrace_offset, id_field.ftrace_type);
  message->AppendVarInt(id_field.proto_field_id, syscall_id);
  //    field:long ret;
  if (ret_field.ftrace_type != kFtraceInt32 &&
      ret_field.ftrace_type != kFtraceInt64) {
    return false;
  }
  const int64_t syscall_ret = ReadSignedFtraceValue(
      start + ret_field.ftrace_offset, ret_field.ftrace_type);
  message->AppendVarInt(ret_field.proto_field_id, syscall_ret);
  // for any syscalls which return a new filedescriptor
  // we mark the fd as potential candidate for scraping
  // if the call succeeded and is within fd bounds
  if (ds_config->syscalls_returning_fd.count(syscall_id) && syscall_ret >= 0 &&
      syscall_ret <= std::numeric_limits<int>::max()) {
    const auto pid = metadata->last_seen_common_pid;
    const auto syscall_ret_u = static_cast<uint64_t>(syscall_ret);
    metadata->fds.insert(std::make_pair(pid, syscall_ret_u));
  }
  return true;
}

// Parse a sched_switch event according to pre-validated format, and buffer the
// individual fields in the current compact batch. See the code populating
// |CompactSchedSwitchFormat| for the assumptions made around the format, which
// this code is closely tied to.
// static
void CpuReader::ParseSchedSwitchCompact(const uint8_t* start,
                                        uint64_t timestamp,
                                        const CompactSchedSwitchFormat* format,
                                        CompactSchedBuffer* compact_buf,
                                        FtraceMetadata* metadata) {
  compact_buf->sched_switch().AppendTimestamp(timestamp);

  int32_t next_pid = ReadValue<int32_t>(start + format->next_pid_offset);
  compact_buf->sched_switch().next_pid().Append(next_pid);
  metadata->AddPid(next_pid);

  int32_t next_prio = ReadValue<int32_t>(start + format->next_prio_offset);
  compact_buf->sched_switch().next_prio().Append(next_prio);

  // Varint encoding of int32 and int64 is the same, so treat the value as
  // int64 after reading.
  int64_t prev_state = ReadSignedFtraceValue(start + format->prev_state_offset,
                                             format->prev_state_type);
  compact_buf->sched_switch().prev_state().Append(prev_state);

  // next_comm
  const char* comm_ptr =
      reinterpret_cast<const char*>(start + format->next_comm_offset);
  size_t iid = compact_buf->interner().InternComm(comm_ptr);
  compact_buf->sched_switch().next_comm_index().Append(iid);
}

// static
void CpuReader::ParseSchedWakingCompact(const uint8_t* start,
                                        uint64_t timestamp,
                                        const CompactSchedWakingFormat* format,
                                        CompactSchedBuffer* compact_buf,
                                        FtraceMetadata* metadata) {
  compact_buf->sched_waking().AppendTimestamp(timestamp);

  int32_t pid = ReadValue<int32_t>(start + format->pid_offset);
  compact_buf->sched_waking().pid().Append(pid);
  metadata->AddPid(pid);

  int32_t target_cpu = ReadValue<int32_t>(start + format->target_cpu_offset);
  compact_buf->sched_waking().target_cpu().Append(target_cpu);

  int32_t prio = ReadValue<int32_t>(start + format->prio_offset);
  compact_buf->sched_waking().prio().Append(prio);

  // comm
  const char* comm_ptr =
      reinterpret_cast<const char*>(start + format->comm_offset);
  size_t iid = compact_buf->interner().InternComm(comm_ptr);
  compact_buf->sched_waking().comm_index().Append(iid);
}

}  // namespace perfetto
