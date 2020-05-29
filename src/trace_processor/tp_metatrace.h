/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TP_METATRACE_H_
#define SRC_TRACE_PROCESSOR_TP_METATRACE_H_

#include <array>
#include <functional>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/metatrace_events.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/thread_checker.h"

// Trace processor maintains its own base implementation to avoid the
// threading and task runners which are required by base's metatracing.
// Moreover, this metatrace also adds support for args which is missing
// from base's metatracing.
// On the other hand, this implementation is not (currently) thread-safe
// and is likely less performant than base's implementation.
namespace perfetto {
namespace trace_processor {
namespace metatrace {

// Stores whether meta-tracing is enabled.
extern bool g_enabled;

inline uint64_t TraceTimeNowNs() {
  return static_cast<uint64_t>(base::GetBootTimeNs().count());
}

struct Record {
  // Timestamp since boot in ns.
  uint64_t timestamp_ns;

  // Duration of the event.
  uint32_t duration_ns;

  // The name of the event.
  // This is assumed to be a static/long lived string.
  const char* event_name;

  // Extra context for some types of events.
  // This buffer is leaked once per record - every time a record is
  // reused, the old memory is released and a new allocation is performed.
  char* args_buffer = nullptr;
  uint32_t args_buffer_size = 0;

  // Adds an arg to the record.
  void AddArg(base::StringView key, base::StringView value) {
    size_t new_buffer_size = args_buffer_size + key.size() + value.size() + 2;
    args_buffer = static_cast<char*>(realloc(args_buffer, new_buffer_size));

    memcpy(&args_buffer[args_buffer_size], key.data(), key.size());
    args_buffer[args_buffer_size + key.size()] = '\0';
    memcpy(&args_buffer[args_buffer_size + key.size() + 1], value.data(),
           value.size());
    args_buffer[new_buffer_size - 1] = '\0';

    args_buffer_size = static_cast<uint32_t>(new_buffer_size);
  }

  void AddArg(base::StringView key, const std::string& value) {
    AddArg(key, base::StringView(value));
  }
};

// Implementation of fixed-size ring buffer. The implementation of this
// class is modelled on the RingBuffer in metatrace.h of base but is different
// in a couple of ways:
//  1. This class is *not* thread safe.
//  2. The Record type stored in this class has the capability of storing
//     extra, event-specific context. For example, when tracing SQL query
//     execution, we store the query string.
//  3. The buffer is designed to be written continuously while meta-tracing
//     is enabled and read one-shot at the end of execution.
class RingBuffer {
 public:
  static constexpr uint32_t kCapacity = 256 * 1024;

  RingBuffer();
  ~RingBuffer() = default;

  std::pair<uint64_t, Record*> AppendRecord(const char* event_name) {
    PERFETTO_DCHECK_THREAD(thread_checker_);
    PERFETTO_DCHECK(!is_reading_);

    uint64_t idx = write_idx_++;
    Record* record = At(idx);
    record->timestamp_ns = TraceTimeNowNs();
    record->duration_ns = 0;
    record->event_name = event_name;
    record->args_buffer_size = 0;
    return std::make_pair(idx, record);
  }

  Record* At(uint64_t idx) { return &data_[idx % kCapacity]; }

  void ReadAll(std::function<void(Record*)>);

  static RingBuffer* GetInstance() {
    static RingBuffer* rb = new RingBuffer();
    return rb;
  }

  uint64_t IndexOf(Record* record) {
    return static_cast<uint64_t>(std::distance(data_.data(), record));
  }

  // Returns whether the record at the |index| has been overwritten because
  // of wraps of the ring buffer.
  bool HasOverwritten(uint64_t index) { return index + kCapacity < write_idx_; }

 private:
  bool is_reading_ = false;

  uint64_t start_idx_ = 0;
  uint64_t write_idx_ = 0;
  std::array<Record, kCapacity> data_;

  PERFETTO_THREAD_CHECKER(thread_checker_)
};

class ScopedEvent {
 public:
  ScopedEvent() = default;

  ~ScopedEvent() {
    if (PERFETTO_LIKELY(!record_))
      return;
    if (RingBuffer::GetInstance()->HasOverwritten(record_idx_))
      return;
    auto now = TraceTimeNowNs();
    record_->duration_ns = static_cast<uint32_t>(now - record_->timestamp_ns);
  }

  ScopedEvent(ScopedEvent&& value) {
    record_ = value.record_;
    record_idx_ = value.record_idx_;
    value.record_ = nullptr;
  }

  template <typename Fn = void(Record*)>
  static ScopedEvent Create(
      const char* event_id,
      Fn args_fn = [](Record*) {}) {
    if (PERFETTO_LIKELY(!g_enabled))
      return ScopedEvent();

    ScopedEvent event;
    std::tie(event.record_idx_, event.record_) =
        RingBuffer::GetInstance()->AppendRecord(event_id);
    args_fn(event.record_);
    return event;
  }

 private:
  ScopedEvent(const ScopedEvent&) = delete;
  ScopedEvent& operator=(const ScopedEvent&) = delete;

  ScopedEvent& operator=(ScopedEvent&& value) = delete;

  Record* record_ = nullptr;
  uint64_t record_idx_ = 0;
};

// Enables meta-tracing of trace-processor.
void Enable();

// Disables meta-tracing of trace-processor and reads all records.
void DisableAndReadBuffer(std::function<void(Record*)>);

// Boilerplate to derive a unique variable name for the event.
#define PERFETTO_TP_METATRACE_UID2(a, b) a##b
#define PERFETTO_TP_METATRACE_UID(x) PERFETTO_TP_METATRACE_UID2(metatrace_, x)

#define PERFETTO_TP_TRACE(...)                  \
  auto PERFETTO_TP_METATRACE_UID(__COUNTER__) = \
      ::perfetto::trace_processor::metatrace::ScopedEvent::Create(__VA_ARGS__)

}  // namespace metatrace
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TP_METATRACE_H_
