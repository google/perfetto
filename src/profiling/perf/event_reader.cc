/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/perf/event_reader.h"

#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "perfetto/ext/base/utils.h"

namespace perfetto {
namespace profiling {

namespace {

template <typename T>
const char* ReadValue(T* value_out, const char* ptr) {
  memcpy(value_out, reinterpret_cast<const void*>(ptr), sizeof(T));
  return ptr + sizeof(T);
}

bool IsPowerOfTwo(size_t v) {
  return (v != 0 && ((v & (v - 1)) == 0));
}

static int perf_event_open(perf_event_attr* attr,
                           pid_t pid,
                           int cpu,
                           int group_fd,
                           unsigned long flags) {
  return static_cast<int>(
      syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags));
}

base::ScopedFile PerfEventOpen(const EventConfig& event_cfg) {
  base::ScopedFile perf_fd{
      perf_event_open(event_cfg.perf_attr(), event_cfg.target_tid(),
                      /*cpu=*/-1, /*group_fd=*/-1, PERF_FLAG_FD_CLOEXEC)};
  return perf_fd;
}

}  // namespace

PerfRingBuffer::PerfRingBuffer(PerfRingBuffer&& other) noexcept
    : metadata_page_(other.metadata_page_),
      mmap_sz_(other.mmap_sz_),
      data_buf_(other.data_buf_),
      data_buf_sz_(other.data_buf_sz_) {
  other.metadata_page_ = nullptr;
  other.mmap_sz_ = 0;
  other.data_buf_ = nullptr;
  other.data_buf_sz_ = 0;
}

PerfRingBuffer& PerfRingBuffer::operator=(PerfRingBuffer&& other) noexcept {
  if (this == &other)
    return *this;

  this->~PerfRingBuffer();
  new (this) PerfRingBuffer(std::move(other));
  return *this;
}

PerfRingBuffer::~PerfRingBuffer() {
  if (!valid())
    return;

  if (munmap(reinterpret_cast<void*>(metadata_page_), mmap_sz_) != 0)
    PERFETTO_PLOG("failed munmap");
}

base::Optional<PerfRingBuffer> PerfRingBuffer::Allocate(
    int perf_fd,
    size_t data_page_count) {
  // perf_event_open requires the ring buffer to be a power of two in size.
  PERFETTO_CHECK(IsPowerOfTwo(data_page_count));

  PerfRingBuffer ret;

  // mmap request is one page larger than the buffer size (for the metadata).
  ret.data_buf_sz_ = data_page_count * base::kPageSize;
  ret.mmap_sz_ = ret.data_buf_sz_ + base::kPageSize;

  // If PROT_WRITE, kernel won't overwrite unread samples.
  void* mmap_addr = mmap(nullptr, ret.mmap_sz_, PROT_READ | PROT_WRITE,
                         MAP_SHARED, perf_fd, 0);
  if (mmap_addr == MAP_FAILED) {
    PERFETTO_PLOG("failed mmap (check perf_event_mlock_kb in procfs)");
    return base::nullopt;
  }

  // Expected layout is [ metadata page ] [ data pages ... ]
  ret.metadata_page_ = reinterpret_cast<perf_event_mmap_page*>(mmap_addr);
  ret.data_buf_ = reinterpret_cast<char*>(mmap_addr) + base::kPageSize;
  PERFETTO_CHECK(ret.metadata_page_->data_offset == base::kPageSize);
  PERFETTO_CHECK(ret.metadata_page_->data_size = ret.data_buf_sz_);

  return base::make_optional(std::move(ret));
}

// TODO(rsavitski): look into more specific barrier builtins. Copying simpleperf
// for now. See |perf_output_put_handle| in the kernel for the barrier
// requirements.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Watomic-implicit-seq-cst"
#endif
std::vector<char> PerfRingBuffer::ReadAvailable() {
  if (!valid())
    return {};

  uint64_t write_offset = metadata_page_->data_head;
  uint64_t read_offset = metadata_page_->data_tail;
  __sync_synchronize();  // needs to be rmb()

  size_t read_pos = static_cast<size_t>(read_offset & (data_buf_sz_ - 1));
  size_t data_sz = static_cast<size_t>(write_offset - read_offset);

  if (data_sz == 0) {
    return {};
  }

  // memcpy accounting for wrapping
  std::vector<char> data(data_sz);
  size_t copy_sz = std::min(data_sz, data_buf_sz_ - read_pos);
  memcpy(data.data(), data_buf_ + read_pos, copy_sz);
  if (copy_sz < data_sz) {
    memcpy(data.data() + copy_sz, data_buf_, data_sz - copy_sz);
  }

  // consume the data
  __sync_synchronize();  // needs to be mb()
  metadata_page_->data_tail += data_sz;

  PERFETTO_LOG("WIP: consumed [%zu] bytes from ring buffer", data_sz);
  return data;
}
#pragma GCC diagnostic pop

EventReader::EventReader(const EventConfig& event_cfg,
                         base::ScopedFile perf_fd,
                         PerfRingBuffer ring_buffer)
    : event_cfg_(event_cfg),
      perf_fd_(std::move(perf_fd)),
      ring_buffer_(std::move(ring_buffer)) {}

EventReader::EventReader(EventReader&& other) noexcept
    : event_cfg_(other.event_cfg_),
      perf_fd_(std::move(other.perf_fd_)),
      ring_buffer_(std::move(other.ring_buffer_)) {}

EventReader& EventReader::operator=(EventReader&& other) noexcept {
  if (this == &other)
    return *this;

  this->~EventReader();
  new (this) EventReader(std::move(other));
  return *this;
}

base::Optional<EventReader> EventReader::ConfigureEvents(
    const EventConfig& event_cfg) {
  auto perf_fd = PerfEventOpen(event_cfg);
  if (!perf_fd) {
    PERFETTO_PLOG("failed perf_event_open");
    return base::nullopt;
  }

  auto ring_buffer =
      PerfRingBuffer::Allocate(perf_fd.get(), /*data_page_count=*/128);
  if (!ring_buffer.has_value()) {
    return base::nullopt;
  }

  return base::make_optional<EventReader>(event_cfg, std::move(perf_fd),
                                          std::move(ring_buffer.value()));
}

void EventReader::ParseNextSampleBatch() {
  std::vector<char> data = ring_buffer_.ReadAvailable();
  if (data.size() == 0) {
    PERFETTO_LOG("WIP: no samples");
    return;
  }

  for (const char* ptr = data.data(); ptr < data.data() + data.size();) {
    if (!ParseSampleAndAdvance(&ptr))
      break;
  }
}

bool EventReader::ParseSampleAndAdvance(const char** ptr) {
  const char* sample_start = *ptr;
  auto* event_hdr = reinterpret_cast<const perf_event_header*>(sample_start);

  PERFETTO_LOG("WIP: event_header[%zu][%zu][%zu]",
               static_cast<size_t>(event_hdr->type),
               static_cast<size_t>(event_hdr->misc),
               static_cast<size_t>(event_hdr->size));

  if (event_hdr->type == PERF_RECORD_SAMPLE) {
    ParsePerfRecordSample(sample_start, event_hdr->size);
  } else {
    PERFETTO_ELOG("WIP: unsupported event type");
  }

  *ptr = sample_start + event_hdr->size;
  return true;
}

// TODO(rsavitski): actually handle the samples instead of logging.
void EventReader::ParsePerfRecordSample(const char* sample_start,
                                        size_t sample_size) {
  const perf_event_attr* cfg = event_cfg_.perf_attr();

  if (cfg->sample_type &
      (~uint64_t(PERF_SAMPLE_TID | PERF_SAMPLE_STACK_USER))) {
    PERFETTO_ELOG("WIP: unsupported sampling option.");
    return;
  }

  // Parse the payload, which consists of concatenated data for each
  // |attr.sample_type| flag.
  const char* parse_pos = sample_start + sizeof(perf_event_header);

  if (cfg->sample_type & PERF_SAMPLE_TID) {
    uint32_t pid;
    parse_pos = ReadValue(&pid, parse_pos);
    PERFETTO_LOG("pid: %" PRIu32 "", pid);

    uint32_t tid;
    parse_pos = ReadValue(&tid, parse_pos);
    PERFETTO_LOG("tid: %" PRIu32 "", tid);
  }

  if (cfg->sample_type & PERF_SAMPLE_STACK_USER) {
    uint64_t max_stack_size;  // the requested size
    parse_pos = ReadValue(&max_stack_size, parse_pos);
    PERFETTO_LOG("max_stack_size: %" PRIu64 "", max_stack_size);

    parse_pos += max_stack_size;  // skip raw data

    // not written if requested stack sampling size is zero
    if (max_stack_size > 0) {
      uint64_t filled_stack_size;
      parse_pos = ReadValue(&filled_stack_size, parse_pos);
      PERFETTO_LOG("filled_stack_size: %" PRIu64 "", filled_stack_size);
    }
  }

  PERFETTO_CHECK(parse_pos == sample_start + sample_size);
}

}  // namespace profiling
}  // namespace perfetto
