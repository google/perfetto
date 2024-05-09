/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_PROCESS_THREAD_TIMELINE_H_
#define SRC_TRACE_REDACTION_PROCESS_THREAD_TIMELINE_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace perfetto::trace_redaction {

class ProcessThreadTimeline {
 public:
  // Opened and closed events are used to mark the start and end of lifespans.
  class Event {
   public:
    enum class Type { kInvalid, kOpen, kClose };

    Event()
        : type_(ProcessThreadTimeline::Event::Type::kInvalid),
          ts_(0),
          pid_(0),
          ppid_(0),
          uid_(0) {}

    Type type() const { return type_; }

    uint64_t ts() const { return ts_; }

    int32_t pid() const { return pid_; }

    int32_t ppid() const { return ppid_; }

    uint64_t uid() const { return uid_; }

    bool operator==(const Event& o) const {
      switch (type_) {
        case Type::kOpen:
          return o.type_ == Type::kOpen && ts_ == o.ts_ && pid_ == o.pid_ &&
                 ppid_ == o.ppid_ && uid_ == o.uid_;

        case Type::kClose:
          return o.type_ == Type::kClose && ts_ == o.ts_ && pid_ == o.pid_;

        case Type::kInvalid:
          return o.type_ == Type::kInvalid;
      }

      return false;
    }

    bool operator!=(const Event& o) const { return !(*this == o); }

    static Event Open(uint64_t ts, int32_t pid, int32_t ppid, uint64_t uid) {
      return Event(ProcessThreadTimeline::Event::Type::kOpen, ts, pid, ppid,
                   uid);
    }

    static Event Open(uint64_t ts, int32_t pid, int32_t ppid) {
      return Event(ProcessThreadTimeline::Event::Type::kOpen, ts, pid, ppid, 0);
    }

    static Event Close(uint64_t ts, int32_t pid) {
      return Event(ProcessThreadTimeline::Event::Type::kClose, ts, pid, 0, 0);
    }

   private:
    Event(Type type, uint64_t ts, int32_t pid, int32_t ppid, uint64_t uid)
        : type_(type), ts_(ts), pid_(pid), ppid_(ppid), uid_(uid) {}

    Type type_ = Type::kInvalid;

    // Valid: open & close
    uint64_t ts_ = 0;

    // Valid: open & close
    int32_t pid_ = -1;

    // Valid: open
    int32_t ppid_ = -1;

    // Valid: open
    uint64_t uid_ = 0;
  };

  // The state of a process at a specific point in time.
  struct Slice {
    int32_t pid = -1;

    // It is safe to use 0 as the invalid value because that's effectively
    // what happening in the trace.
    uint64_t uid = 0;
  };

  ProcessThreadTimeline() = default;

  ProcessThreadTimeline(const ProcessThreadTimeline&) = delete;
  ProcessThreadTimeline& operator=(const ProcessThreadTimeline&) = delete;
  ProcessThreadTimeline(ProcessThreadTimeline&&) = delete;
  ProcessThreadTimeline& operator=(ProcessThreadTimeline&&) = delete;

  void Append(const Event& event);

  // REQUIRED: Sorts all events by pid, making it possible to locate the subset
  // of events connected to a pid. Events are not sorted by time because the
  // subset of events will, on average, be trivally small.
  void Sort();

  // Returns a snapshot that contains a process's pid and ppid, but contains the
  // first uid found in its parent-child chain. If a uid cannot be found, uid=0
  // is returned.
  //
  // `Sort()` must be called before this.
  Slice Search(uint64_t ts, int32_t pid) const;

 private:
  enum class Mode {
    // The timeline can safely be queried. If the timeline is in read mode, and
    // a user writes to the timeline, the timeline will change to write mode.
    kRead,

    // The timeline change be changed. If the timeline is not in write mode,
    // reading from the timeline will throw an error. Sort() must be called to
    // change the timeline from write to read mode.
    kWrite
  };

  // Effectively this is the same as:
  //
  //  events_for(pid).before(ts).sort_by_time().last()
  std::optional<Event> FindPreviousEvent(uint64_t ts, int32_t pid) const;

  std::optional<Event> Search(size_t depth, uint64_t ts, int32_t pid) const;

  bool TestEvent(std::optional<Event> event) const;

  std::vector<Event> events_;

  Mode mode_ = Mode::kRead;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_PROCESS_THREAD_TIMELINE_H_
