/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PROCESS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_PROCESS_TRACKER_H_

#include <tuple>

#include "perfetto/base/string_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class ProcessTracker {
 public:
  explicit ProcessTracker(TraceProcessorContext*);
  ProcessTracker(const ProcessTracker&) = delete;
  ProcessTracker& operator=(const ProcessTracker&) = delete;
  virtual ~ProcessTracker();

  using UniqueProcessIterator =
      std::multimap<uint32_t, UniquePid>::const_iterator;
  using UniqueProcessBounds =
      std::pair<UniqueProcessIterator, UniqueProcessIterator>;

  using UniqueThreadIterator =
      std::multimap<uint32_t, UniqueTid>::const_iterator;
  using UniqueThreadBounds =
      std::pair<UniqueThreadIterator, UniqueThreadIterator>;

  // TODO(b/110409911): Invalidation of process and threads is yet to be
  // implemented. This will include passing timestamps into the below methods
  // to ensure the correct upid/utid is found.

  // Called when a task_newtask is observed. This force the tracker to start
  // a new UTID for the thread, which is needed for TID-recycling resolution.
  UniqueTid StartNewThread(int64_t timestamp,
                           uint32_t tid,
                           StringId thread_name_id);

  // Returns the thread utid (or creates a new entry if not present)
  UniqueTid GetOrCreateThread(uint32_t tid);

  // Called when a sched switch event is seen in the trace. Retrieves the
  // UniqueTid that matches the tid or assigns a new UniqueTid and stores
  // the thread_name_id.
  UniqueTid UpdateThreadName(uint32_t tid, StringId thread_name_id);

  // Called when a thread is seen the process tree. Retrieves the matching utid
  // for the tid and the matching upid for the tgid and stores both.
  // Virtual for testing.
  virtual UniqueTid UpdateThread(uint32_t tid, uint32_t tgid);

  // Called when a task_newtask without the CLONE_THREAD flag is observed.
  // This force the tracker to start both a new UTID and a new UPID.
  UniquePid StartNewProcess(int64_t timestamp, uint32_t pid);

  // Called when a process is seen in a process tree. Retrieves the UniquePid
  // for that pid or assigns a new one.
  // Virtual for testing.
  virtual UniquePid UpdateProcess(uint32_t pid,
                                  base::Optional<uint32_t> ppid,
                                  base::StringView name);

  // Called when a process is seen in a process tree. Retrieves the UniquePid
  // for that pid or assigns a new one.
  // Virtual for testing.
  virtual UniquePid GetOrCreateProcess(uint32_t pid);

  // Returns the bounds of a range that includes all UniquePids that have the
  // requested pid.
  UniqueProcessBounds UpidsForPid(uint32_t pid) {
    return pids_.equal_range(pid);
  }

  // Returns the bounds of a range that includes all UniqueTids that have the
  // requested tid.
  UniqueThreadBounds UtidsForTid(uint32_t tid) {
    return tids_.equal_range(tid);
  }

  // Marks the two threads as belonging to the same process, even if we don't
  // know which one yet. If one of the two threads is later mapped to a process,
  // the other will be mapped to the same process. The order of the two threads
  // is irrelevant, Associate(A, B) has the same effect of Associate(B, A).
  void AssociateThreads(UniqueTid, UniqueTid);

 private:
  // Called whenever we discover that the passed thread belongs to the passed
  // process. The |pending_assocs_| vector is scanned to see if there are any
  // other threads associated to the passed thread.
  void ResolvePendingAssociations(UniqueTid, UniquePid);

  std::pair<UniquePid, TraceStorage::Process*> GetOrCreateProcessPtr(
      uint32_t pid);

  TraceProcessorContext* const context_;

  // Each tid can have multiple UniqueTid entries, a new UniqueTid is assigned
  // each time a thread is seen in the trace.
  std::multimap<uint32_t /* tid */, UniqueTid> tids_;

  // Each pid can have multiple UniquePid entries, a new UniquePid is assigned
  // each time a process is seen in the trace.
  std::map<uint32_t /* pid (aka tgid) */, UniquePid> pids_;

  // Pending thread associations. The meaning of a pair<ThreadA, ThreadB> in
  // this vector is: we know that A and B belong to the same process, but we
  // don't know yet which process. A and A are idempotent, as in, pair<A,B> is
  // equivalent to pair<B,A>.
  std::vector<std::pair<UniqueTid, UniqueTid>> pending_assocs_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROCESS_TRACKER_H_
