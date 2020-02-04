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

#ifndef SRC_PROFILING_PERF_EVENT_READER_H_
#define SRC_PROFILING_PERF_EVENT_READER_H_

#include <linux/perf_event.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "src/profiling/perf/event_config.h"

namespace perfetto {
namespace profiling {

class PerfRingBuffer {
 public:
  static base::Optional<PerfRingBuffer> Allocate(int perf_fd,
                                                 size_t data_page_count);

  ~PerfRingBuffer();

  // move-only
  PerfRingBuffer(const PerfRingBuffer&) = delete;
  PerfRingBuffer& operator=(const PerfRingBuffer&) = delete;
  PerfRingBuffer(PerfRingBuffer&& other) noexcept;
  PerfRingBuffer& operator=(PerfRingBuffer&& other) noexcept;

  char* ReadRecordNonconsuming();
  void Consume(size_t bytes);

 private:
  PerfRingBuffer() = default;

  bool valid() const { return metadata_page_ != nullptr; }

  // TODO(rsavitski): volatile?
  // Points at the start of the mmap'd region.
  perf_event_mmap_page* metadata_page_ = nullptr;

  // Size of the mmap'd region (1 metadata page + data_buf_sz_).
  size_t mmap_sz_ = 0;

  // mmap'd ring buffer
  char* data_buf_ = nullptr;
  size_t data_buf_sz_ = 0;

  // When a record wraps around the ring buffer boundary, it is reconstructed in
  // a contiguous form in this buffer. This allows us to always return a pointer
  // to a contiguous record.
  constexpr static size_t kMaxPerfRecordSize = 1 << 16;  // max size 64k
  alignas(uint64_t) char reconstructed_record_[kMaxPerfRecordSize];
};

struct ParsedSample {
  pid_t pid = 0;
  pid_t tid = 0;
  uint64_t timestamp = 0;
  std::unique_ptr<unwindstack::Regs> regs;
  std::vector<char> stack;
};

class EventReader {
 public:
  // Allow base::Optional<EventReader> without making the constructor public.
  template <typename EventReader, bool>
  friend struct base::internal::OptionalStorageBase;

  static base::Optional<EventReader> ConfigureEvents(
      const EventConfig& event_cfg);

  // Consumes records from the ring buffer until either encountering a sample,
  // or catching up to the writer. The other record of interest
  // (PERF_RECORD_LOST) is handled via the given callback.
  base::Optional<ParsedSample> ReadUntilSample(
      std::function<void(uint64_t)> lost_events_callback);

  ~EventReader() = default;

  // move-only
  EventReader(const EventReader&) = delete;
  EventReader& operator=(const EventReader) = delete;
  EventReader(EventReader&&) noexcept;
  EventReader& operator=(EventReader&&) noexcept;

 private:
  EventReader(const EventConfig& event_cfg,
              base::ScopedFile perf_fd,
              PerfRingBuffer ring_buffer);

  ParsedSample ParseSampleRecord(const char* sample_payload,
                                 size_t sample_size);

  const EventConfig event_cfg_;
  base::ScopedFile perf_fd_;
  PerfRingBuffer ring_buffer_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_EVENT_READER_H_
