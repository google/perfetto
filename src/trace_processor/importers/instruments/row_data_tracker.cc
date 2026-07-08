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

#include "src/trace_processor/importers/instruments/row_data_tracker.h"

#include "perfetto/base/status.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_TP_INSTRUMENTS)
#error \
    "This file should not be built when enable_perfetto_trace_processor_mac_instruments=false"
#endif

namespace perfetto::trace_processor::instruments_importer {

namespace {

// Resolves an id (as handed out by RowDataTracker::New*()) back to a pointer
// into `entries`, or nullptr if `id` doesn't refer to a live entry.
//
// The XML file format allows a `ref="N"` attribute to reference an entry
// declared earlier via `id="N"`. Because the id -> entry mapping is derived
// from an untrusted, attacker-controlled trace file, `id` must never be
// trusted blindly: a `ref` that was never registered (or a tag missing its
// id/ref attribute entirely) can otherwise surface here as `kNullId` or as a
// stale/out-of-range value. Indexing `entries` without validating `id` first
// is an out-of-bounds read (and, transitively, an out-of-bounds write via
// whatever field callers subsequently assign through the returned pointer).
template <typename T, typename Id>
T* At(std::vector<T>& entries, Id id) {
  if (id == kNullId || id > entries.size()) {
    return nullptr;
  }
  return &entries[id - 1];
}

}  // namespace

RowDataTracker::RowDataTracker() {}
RowDataTracker::~RowDataTracker() = default;

IdPtr<Thread> RowDataTracker::NewThread() {
  ThreadId id = static_cast<ThreadId>(threads_.size());
  Thread* ptr = &threads_.emplace_back();
  // Always add 1 to ids, so that they're non-zero.
  return {id + 1, ptr};
}
Thread* RowDataTracker::GetThread(ThreadId id) {
  return At(threads_, id);
}

IdPtr<Process> RowDataTracker::NewProcess() {
  ProcessId id = static_cast<ProcessId>(processes_.size());
  Process* ptr = &processes_.emplace_back();
  // Always add 1 to ids, so that they're non-zero.
  return {id + 1, ptr};
}
Process* RowDataTracker::GetProcess(ProcessId id) {
  return At(processes_, id);
}

IdPtr<Frame> RowDataTracker::NewFrame() {
  BacktraceFrameId id = static_cast<BacktraceFrameId>(frames_.size());
  Frame* ptr = &frames_.emplace_back();
  // Always add 1 to ids, so that they're non-zero.
  return {id + 1, ptr};
}
Frame* RowDataTracker::GetFrame(BacktraceFrameId id) {
  return At(frames_, id);
}

IdPtr<Backtrace> RowDataTracker::NewBacktrace() {
  BacktraceId id = static_cast<BacktraceId>(backtraces_.size());
  Backtrace* ptr = &backtraces_.emplace_back();
  // Always add 1 to ids, so that they're non-zero.
  return {id + 1, ptr};
}
Backtrace* RowDataTracker::GetBacktrace(BacktraceId id) {
  return At(backtraces_, id);
}

IdPtr<Binary> RowDataTracker::NewBinary() {
  BinaryId id = static_cast<BinaryId>(binaries_.size());
  Binary* ptr = &binaries_.emplace_back();
  // Always add 1 to ids, so that they're non-zero.
  return {id + 1, ptr};
}
Binary* RowDataTracker::GetBinary(BinaryId id) {
  // Frames are allowed to have null binaries.
  return At(binaries_, id);
}

}  // namespace perfetto::trace_processor::instruments_importer
