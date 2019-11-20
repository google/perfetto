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
#include "src/profiling/perf/event_config.h"

namespace perfetto {
namespace profiling {

// TODO(rsavitski): currently written for the non-overwriting ring buffer mode
// (PROT_WRITE). Decide on whether there are use-cases for supporting the other.
// TODO(rsavitski): given perf_event_mlock_kb limit, can we afford a ring buffer
// per data source, or will we be forced to multiplex everything onto a single
// ring buffer in the worst case? Alternatively, obtain CAP_IPC_LOCK (and do own
// limiting)? Or get an adjusted RLIMIT_MEMLOCK?
// TODO(rsavitski): polling for now, look into supporting the notification
// mechanisms (such as epoll) later.
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

  std::vector<char> ReadAvailable();

 private:
  PerfRingBuffer() = default;

  bool valid() const { return metadata_page_ != nullptr; }

  // TODO(rsavitski): volatile?
  // Is exactly the start of the mmap'd region.
  perf_event_mmap_page* metadata_page_ = nullptr;

  // size of the mmap'd region (1 metadata page + data_buf_sz_)
  size_t mmap_sz_ = 0;

  // mmap'd ring buffer
  char* data_buf_ = nullptr;
  size_t data_buf_sz_ = 0;
};

class EventReader {
 public:
  // Allow base::Optional<EventReader> without making the constructor public.
  template <typename EventReader, bool>
  friend struct base::internal::OptionalStorageBase;

  static base::Optional<EventReader> ConfigureEvents(
      const EventConfig& event_cfg);

  ~EventReader() = default;

  // move-only
  EventReader(const EventReader&) = delete;
  EventReader& operator=(const EventReader) = delete;
  EventReader(EventReader&&) noexcept;
  EventReader& operator=(EventReader&&) noexcept;

  // TODO(rsavitski): temporary.
  void ParseNextSampleBatch();

 private:
  EventReader(const EventConfig& event_cfg,
              base::ScopedFile perf_fd,
              PerfRingBuffer ring_buffer);

  bool ParseSampleAndAdvance(const char** ptr);
  void ParsePerfRecordSample(const char* sample_payload, size_t sample_size);

  const EventConfig event_cfg_;
  base::ScopedFile perf_fd_;
  PerfRingBuffer ring_buffer_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_EVENT_READER_H_
