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

#include "src/trace_processor/trace_processor_storage_impl.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/binder_tracker.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/heap_profile_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_module.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/proto/android_probes_module.h"
#include "src/trace_processor/importers/proto/graphics_event_module.h"
#include "src/trace_processor/importers/proto/heap_graph_module.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"
#include "src/trace_processor/importers/proto/system_probes_module.h"
#include "src/trace_processor/importers/proto/track_event_module.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/stack_profile_tracker.h"
#include "src/trace_processor/syscall_tracker.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/track_tracker.h"
#include "src/trace_processor/vulkan_memory_tracker.h"

namespace perfetto {
namespace trace_processor {

TraceProcessorStorageImpl::TraceProcessorStorageImpl(const Config& cfg) {
  context_.config = cfg;
  context_.storage.reset(new TraceStorage(context_.config));
  context_.track_tracker.reset(new TrackTracker(&context_));
  context_.args_tracker.reset(new ArgsTracker(&context_));
  context_.slice_tracker.reset(new SliceTracker(&context_));
  context_.event_tracker.reset(new EventTracker(&context_));
  context_.process_tracker.reset(new ProcessTracker(&context_));
#if PERFETTO_BUILDFLAG(PERFETTO_TP_SYSCALLS)
  context_.syscall_tracker.reset(new SyscallTracker(&context_));
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_SYSCALLS)
  context_.clock_tracker.reset(new ClockTracker(&context_));
  context_.heap_profile_tracker.reset(new HeapProfileTracker(&context_));
#if PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)
  context_.sched_tracker.reset(new SchedEventTracker(&context_));
  context_.systrace_parser.reset(new SystraceParser(&context_));
  context_.binder_tracker.reset(new BinderTracker(&context_));
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)
#if PERFETTO_BUILDFLAG(PERFETTO_TP_GRAPHICS)
  context_.vulkan_memory_tracker.reset(new VulkanMemoryTracker(&context_));
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_GRAPHICS)
  context_.ftrace_module.reset(
      new ProtoImporterModule<FtraceModule>(&context_));
  context_.track_event_module.reset(
      new ProtoImporterModule<TrackEventModule>(&context_));
  context_.system_probes_module.reset(
      new ProtoImporterModule<SystemProbesModule>(&context_));
  context_.android_probes_module.reset(
      new ProtoImporterModule<AndroidProbesModule>(&context_));
  context_.heap_graph_module.reset(
      new ProtoImporterModule<HeapGraphModule>(&context_));
  context_.graphics_event_module.reset(
      new ProtoImporterModule<GraphicsEventModule>(&context_));
}

TraceProcessorStorageImpl::~TraceProcessorStorageImpl() {}

util::Status TraceProcessorStorageImpl::Parse(std::unique_ptr<uint8_t[]> data,
                                              size_t size) {
  if (size == 0)
    return util::OkStatus();
  if (unrecoverable_parse_error_)
    return util::ErrStatus(
        "Failed unrecoverably while parsing in a previous Parse call");
  if (!context_.chunk_reader)
    context_.chunk_reader.reset(new ForwardingTraceParser(&context_));

  auto scoped_trace = context_.storage->TraceExecutionTimeIntoStats(
      stats::parse_trace_duration_ns);
  util::Status status = context_.chunk_reader->Parse(std::move(data), size);
  unrecoverable_parse_error_ |= !status.ok();
  return status;
}

void TraceProcessorStorageImpl::NotifyEndOfFile() {
  if (unrecoverable_parse_error_ || !context_.chunk_reader)
    return;

  if (context_.sorter)
    context_.sorter->ExtractEventsForced();
#if PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)
  context_.sched_tracker->FlushPendingEvents();
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)
  context_.event_tracker->FlushPendingEvents();
  context_.slice_tracker->FlushPendingSlices();
}

}  // namespace trace_processor
}  // namespace perfetto
