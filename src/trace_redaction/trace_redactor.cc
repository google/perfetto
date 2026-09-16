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

#include "src/trace_redaction/trace_redactor.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_redaction/add_synth_threads_to_process_trees.h"
#include "src/trace_redaction/broadphase_packet_filter.h"
#include "src/trace_redaction/collect_clocks.h"
#include "src/trace_redaction/collect_frame_cookies.h"
#include "src/trace_redaction/collect_system_info.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/drop_empty_ftrace_events.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/merge_process_tree.h"
#include "src/trace_redaction/merge_threads.h"
#include "src/trace_redaction/populate_allow_lists.h"
#include "src/trace_redaction/prune_package_list.h"
#include "src/trace_redaction/prune_perf_events.h"
#include "src/trace_redaction/redact_ftrace_events.h"
#include "src/trace_redaction/redact_process_events.h"
#include "src/trace_redaction/reduce_threads_in_process_trees.h"
#include "src/trace_redaction/scrub_process_stats.h"
#include "src/trace_redaction/timeline_validation.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/verify_integrity.h"

#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {

using Trace = protos::pbzero::Trace;
using TracePacket = protos::pbzero::TracePacket;

namespace {

base::StatusOr<trace_processor::TraceBlob> LoadTrace(const std::string& path) {
#if PERFETTO_HAS_MMAP()
  base::ScopedMmap mapped = base::ReadMmapWholeFile(path);
  if (mapped.IsValid()) {
    return trace_processor::TraceBlob::FromMmap(std::move(mapped));
  }
#endif
  std::string contents;
  if (!base::ReadFile(path, &contents)) {
    return base::ErrStatus("TraceRedactor: failed to read trace (%s)",
                           path.c_str());
  }
  return trace_processor::TraceBlob::CopyFrom(contents.data(), contents.size());
}

base::Status WriteTraceToFile(const std::string& path,
                              std::string_view buffer) {
  const auto dest_fd = base::OpenFile(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (dest_fd.get() == -1) {
    return base::ErrStatus(
        "Failed to open destination file; can't write redacted trace.");
  }
  if (const auto exported_data =
          base::WriteAll(dest_fd.get(), buffer.data(), buffer.size());
      exported_data <= 0 && !buffer.empty()) {
    return base::ErrStatus(
        "TraceRedactor: failed to write redacted trace to disk");
  }
  return base::OkStatus();
}

// Appends a packet to the output buffer, prepending the necessary
// trace_packet field tag and length prefix.
void AppendPacketToTrace(std::string_view packet, std::string* output_buffer) {
  uint8_t preamble[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
  uint8_t* ptr = preamble;
  constexpr uint32_t kTag = protozero::proto_utils::MakeTagLengthDelimited(
      protos::pbzero::Trace::kPacketFieldNumber);
  ptr = protozero::proto_utils::WriteVarInt(kTag, ptr);
  ptr = protozero::proto_utils::WriteVarInt(packet.size(), ptr);

  output_buffer->append(reinterpret_cast<const char*>(preamble),
                        static_cast<size_t>(ptr - preamble));
  output_buffer->append(packet.data(), packet.size());
}

}  // namespace

TraceRedactorPass::TraceRedactorPass() = default;

TraceRedactorPass::~TraceRedactorPass() = default;

base::Status TraceRedactorPass::Redact(std::string_view view,
                                       Context* context,
                                       std::string* output_buffer) const {
  RETURN_IF_ERROR(Collect(context, view));
  RETURN_IF_ERROR(Validate(*context));
  RETURN_IF_ERROR(Build(context));
  RETURN_IF_ERROR(Transform(*context, view, output_buffer));
  RETURN_IF_ERROR(Augment(*context, output_buffer));

  return base::OkStatus();
}

base::Status TraceRedactorPass::Collect(Context* context,
                                        std::string_view view) const {
  for (const auto& collector : collectors_) {
    RETURN_IF_ERROR(collector->Begin(context));
  }

  const Trace::Decoder trace_decoder(
      reinterpret_cast<const uint8_t*>(view.data()), view.size());

  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    const TracePacket::Decoder packet(packet_it->as_bytes());

    for (auto& collector : collectors_) {
      RETURN_IF_ERROR(collector->Collect(packet, context));
    }
  }

  for (const auto& collector : collectors_) {
    RETURN_IF_ERROR(collector->End(context));
  }

  return base::OkStatus();
}

base::Status TraceRedactorPass::Validate(const Context& context) const {
  for (const auto& validator : validators_) {
    RETURN_IF_ERROR(validator->Validate(context));
  }
  return base::OkStatus();
}

base::Status TraceRedactorPass::Build(Context* context) const {
  for (const auto& builder : builders_) {
    RETURN_IF_ERROR(builder->Build(context));
  }
  return base::OkStatus();
}

base::Status TraceRedactorPass::Transform(const Context& context,
                                          std::string_view view,
                                          std::string* output_buffer) const {
  const Trace::Decoder trace_decoder(
      reinterpret_cast<const uint8_t*>(view.data()), view.size());
  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    auto packet = packet_it->as_std_string();

    for (const auto& transformer : transformers_) {
      // If the packet has been cleared, it means a transformation has removed
      // it from the trace. Stop processing it. This saves transforms from
      // having to check and handle empty packets.
      if (packet.empty()) {
        break;
      }

      RETURN_IF_ERROR(transformer->Transform(context, &packet));
    }

    // The packet has been removed from the trace. Don't write an empty packet
    // to output.
    if (packet.empty()) {
      continue;
    }

    AppendPacketToTrace(packet, output_buffer);
  }

  return base::OkStatus();
}

base::Status TraceRedactorPass::Augment(const Context& context,
                                        std::string* output_buffer) const {
  std::string packet;
  for (const auto& augmenter : augmenters_) {
    // Keep augmenting until the augmenter returns an empty packet.
    while (true) {
      packet.clear();
      RETURN_IF_ERROR(augmenter->Augment(context, &packet));
      if (packet.empty()) {
        break;
      }
      AppendPacketToTrace(packet, output_buffer);
    }
  }
  return base::OkStatus();
}

TraceRedactor::TraceRedactor() = default;

TraceRedactor::~TraceRedactor() = default;

TraceRedactorPass* TraceRedactor::add_pass() {
  auto pass = std::make_unique<TraceRedactorPass>();
  auto* ptr = pass.get();
  passes_.push_back(std::move(pass));
  return ptr;
}

base::Status TraceRedactor::Redact(std::string_view source_filename,
                                   std::string_view dest_filename,
                                   Context* context) const {
  const std::string source_filename_str(source_filename);
  const std::string dest_filename_str(dest_filename);
  ASSIGN_OR_RETURN(trace_processor::TraceBlob blob,
                   LoadTrace(source_filename_str));

  std::string buffer_a;
  std::string buffer_b;

  std::string_view current_input(reinterpret_cast<const char*>(blob.data()),
                                 blob.size());
  std::string* current_output = &buffer_a;

  for (size_t i = 0; i < passes_.size(); ++i) {
    current_output->clear();
    RETURN_IF_ERROR(passes_[i]->Redact(current_input, context, current_output));
    current_input = *current_output;

    // Double buffering, flip buffers for next pass to only keep at most
    // one buffer for reading and another one for writting, ping-pong between
    // buffer_a and buffer_b.
    current_output = (current_output == &buffer_a) ? &buffer_b : &buffer_a;
  }

  return WriteTraceToFile(dest_filename_str, current_input);
}

std::unique_ptr<TraceRedactor> TraceRedactor::CreateInstance(
    const Config& config) {
  auto redactor = std::make_unique<TraceRedactor>();

  // Pass 1:
  auto* pass1 = redactor->add_pass();

  // VerifyIntegrity breaks the CollectPrimitive pattern. Instead of writing to
  // the context, its job is to read trace packets and return errors if any
  // packet does not look "correct". This primitive is added first in an effort
  // to detect and react to bad input before other collectors run.
  if (config.verify) {
    pass1->emplace_collect<VerifyIntegrity>();
  }

  // Add all collectors.
  pass1->emplace_collect<FindPackageUid>();
  pass1->emplace_collect<CollectTimelineEvents>();
  pass1->emplace_collect<CollectFrameCookies>();
  pass1->emplace_collect<CollectSystemInfo>();
  pass1->emplace_collect<CollectClocks>();

  // Add all validators.
  pass1->emplace_validator<TimelineValidation>();

  // Add all builders.
  pass1->emplace_build<ReduceFrameCookies>();
  pass1->emplace_build<BuildSyntheticThreads>();

  {
    // In order for BroadphasePacketFilter to work, something needs to populate
    // the masks (i.e. PopulateAllowlists).
    pass1->emplace_build<PopulateAllowlists>();
    pass1->emplace_transform<BroadphasePacketFilter>();
  }

  {
    auto* primitive = pass1->emplace_transform<RedactFtraceEvents>();
    primitive->emplace_ftrace_filter<FilterRss>();
    primitive->emplace_post_filter_modifier<DoNothing>();
  }

  {
    auto* primitive = pass1->emplace_transform<RedactFtraceEvents>();
    primitive->emplace_ftrace_filter<FilterFtraceUsingSuspendResume>();
    primitive->emplace_post_filter_modifier<DoNothing>();
  }

  {
    // Remove all frame timeline events that don't belong to the target package.
    pass1->emplace_transform<FilterFrameEvents>();
  }

  pass1->emplace_transform<PrunePackageList>();

  {
    // This primitive has a dependencies on other primitives.
    // The overall flow to make this transform work is as follows:
    //
    // First: CollectClocks retrieves the clock ids to be used for perf samples
    // and sets up the RedactorClockConverter that will handle all the timestamp
    // transformations into trace time which is used by the Timeline.
    //
    // Second: PopulateAllowlists adds the perf samples to be included in the
    // redacted and BroadphasePacketFilter keeps those samples.
    //
    // Third: We emplace the PrunePerfEvents which actually
    // removes the perf samples that don't belong to the target package.
    auto* primitive = pass1->emplace_transform<PrunePerfEvents>();
    primitive->emplace_filter<ConnectedToPackage>();
  }

  // Process stats includes per-process information, such as:
  //
  //   processes {
  //   pid: 1
  //   vm_size_kb: 11716992
  //   vm_rss_kb: 5396
  //   rss_anon_kb: 2896
  //   rss_file_kb: 1728
  //   rss_shmem_kb: 772
  //   vm_swap_kb: 4236
  //   vm_locked_kb: 0
  //   vm_hwm_kb: 6720
  //   oom_score_adj: -1000
  // }
  //
  // Use the ConnectedToPackage primitive to ensure only the target package has
  // stats in the trace.
  {
    auto* primitive = pass1->emplace_transform<ScrubProcessStats>();
    primitive->emplace_filter<ConnectedToPackage>();
  }

  // Redacts all switch and waking events. This should use the same modifier and
  // filter as the process events (see below).
  {
    auto* primitive = pass1->emplace_transform<RedactSchedEvents>();
    primitive->emplace_modifier<ClearComms>();
    primitive->emplace_waking_filter<ConnectedToPackage>();
  }

  // Redacts all new task, rename task, process free events. This should use the
  // same modifier and filter as the schedule events (see above).
  {
    auto* primitive = pass1->emplace_transform<RedactProcessEvents>();
    primitive->emplace_modifier<ClearComms>();
    primitive->emplace_filter<ConnectedToPackage>();
  }

  // Merge Threads (part 1): Remove all waking events that connected to the
  // target package. Change the pids not connected to the target package.
  {
    auto* primitive = pass1->emplace_transform<RedactSchedEvents>();
    primitive->emplace_modifier<MergeThreadsPids>();
    primitive->emplace_waking_filter<ConnectedToPackage>();
  }

  // Merge Threads (part 2): Drop all process events not belonging to the
  // target package. No modification is needed.
  {
    auto* primitive = pass1->emplace_transform<RedactProcessEvents>();
    primitive->emplace_modifier<DoNothing>();
    primitive->emplace_filter<ConnectedToPackage>();
  }

  // Merge Threads (part 3): Replace ftrace event's pid (not the task's pid)
  // for all pids not connected to the target package.
  {
    auto* primitive = pass1->emplace_transform<RedactFtraceEvents>();
    primitive->emplace_post_filter_modifier<MergeThreadsPids>();
    primitive->emplace_ftrace_filter<AllowAll>();
  }

  // Add transforms that will change process trees. The order here matters:
  //
  //  1. Primitives removing processes/threads
  //  2. Primitives adding processes/threads
  //
  // If primitives are not in this order, newly added processes/threads may
  // get removed.
  {
    pass1->emplace_transform<ReduceThreadsInProcessTrees>();
    pass1->emplace_transform<AddSythThreadsToProcessTrees>();
  }

  // Optimizations:
  //
  // This block of transforms should be registered last in Pass 1. They
  // clean-up after the other transforms. The most common function will be to
  // remove empty messages.
  {
    pass1->emplace_transform<DropEmptyFtraceEvents>();
  }

  // Pass 2: Merge process trees into a single deduplicated process tree packet
  // appended at the end of the trace.
  TraceRedactorPass* pass2 = redactor->add_pass();
  pass2->emplace_collect<CollectProcessTrees>();
  pass2->emplace_transform<ReduceProcessTrees>();
  pass2->emplace_augment<AugmentProcessTrees>();

  return redactor;
}

}  // namespace perfetto::trace_redaction
