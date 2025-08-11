/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TYPES_PER_MACHINE_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TYPES_PER_MACHINE_CONTEXT_H_

#include <memory>
#include <optional>

#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/destructible.h"

namespace perfetto::trace_processor {

class CpuTracker;
class MachineTracker;
class MappingTracker;
class ProcessTracker;
class SchedEventTracker;
class TrackCompressor;
class TrackTracker;

using MachineId = tables::MachineTable::Id;

class PerMachineContext {
  std::unique_ptr<TrackCompressor> track_compressor;

  std::unique_ptr<MachineTracker> machine_tracker;
  std::unique_ptr<CpuTracker> cpu_tracker;
  std::unique_ptr<MappingTracker> mapping_tracker;

  // First requested will have exclusive access.
  std::unique_ptr<SchedEventTracker> sched_event_tracker;
  std::unique_ptr<ProcessTracker> process_tracker;
  std::unique_ptr<TrackTracker> track_tracker;

  // These fields are stored as pointers to Destructible objects rather than
  // their actual type (a subclass of Destructible), as the concrete subclass
  // type is only available in storage_full target. To access these fields use
  // the GetOrCreate() method on their subclass type, e.g.
  // SyscallTracker::GetOrCreate(context)
  // clang-format off
  std::unique_ptr<Destructible> binder_tracker;                         // BinderTracker
  std::unique_ptr<Destructible> syscall_tracker;                        // SyscallTracker
  std::unique_ptr<Destructible> system_info_tracker;                    // SystemInfoTracker
  std::unique_ptr<Destructible> ftrace_sched_tracker;                   // FtraceSchedEventTracker
  std::unique_ptr<Destructible> thread_state_tracker;                   // ThreadStateTracker
  std::unique_ptr<Destructible> elf_tracker;                            // ElfTracker
  std::unique_ptr<Destructible> perf_tracker;                           // PerfTracker
  // clang-format on

  std::optional<MachineId> machine_id() const;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TYPES_PER_MACHINE_CONTEXT_H_
