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

#include "src/trace_processor/importers/proto/profile_module.h"
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/deobfuscation_mapping_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/heap_profile_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/importers/proto/profile_packet_utils.h"
#include "src/trace_processor/importers/proto/profiler_util.h"
#include "src/trace_processor/importers/proto/stack_profile_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/stack_traces_util.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/common/perf_events.pbzero.h"
#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/profiling/smaps.pbzero.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;
using protozero::ConstBytes;

ProfileModule::ProfileModule(TraceProcessorContext* context)
    : context_(context) {
  RegisterForField(TracePacket::kStreamingProfilePacketFieldNumber, context);
  RegisterForField(TracePacket::kPerfSampleFieldNumber, context);
  RegisterForField(TracePacket::kProfilePacketFieldNumber, context);
  RegisterForField(TracePacket::kModuleSymbolsFieldNumber, context);
  // note: deobfuscation mappings also handled by HeapGraphModule.
  RegisterForField(TracePacket::kDeobfuscationMappingFieldNumber, context);
  RegisterForField(TracePacket::kSmapsPacketFieldNumber, context);
}

ProfileModule::~ProfileModule() = default;

ModuleResult ProfileModule::TokenizePacket(const TracePacket::Decoder& decoder,
                                           TraceBlobView* packet,
                                           int64_t /*packet_timestamp*/,
                                           PacketSequenceState* state,
                                           uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kStreamingProfilePacketFieldNumber:
      return TokenizeStreamingProfilePacket(state, packet,
                                            decoder.streaming_profile_packet());
  }
  return ModuleResult::Ignored();
}

void ProfileModule::ParseTracePacketData(
    const protos::pbzero::TracePacket::Decoder& decoder,
    int64_t ts,
    const TracePacketData& data,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kStreamingProfilePacketFieldNumber:
      ParseStreamingProfilePacket(ts, data.sequence_state.get(),
                                  decoder.streaming_profile_packet());
      return;
    case TracePacket::kPerfSampleFieldNumber:
      ParsePerfSample(ts, data.sequence_state.get(), decoder);
      return;
    case TracePacket::kProfilePacketFieldNumber:
      ParseProfilePacket(ts, data.sequence_state.get(),
                         decoder.trusted_packet_sequence_id(),
                         decoder.profile_packet());
      return;
    case TracePacket::kModuleSymbolsFieldNumber:
      ParseModuleSymbols(decoder.module_symbols());
      return;
    case TracePacket::kDeobfuscationMappingFieldNumber:
      ParseDeobfuscationMapping(ts, data.sequence_state.get(),
                                decoder.trusted_packet_sequence_id(),
                                decoder.deobfuscation_mapping());
      return;
    case TracePacket::kSmapsPacketFieldNumber:
      ParseSmapsPacket(ts, decoder.smaps_packet());
      return;
  }
}

ModuleResult ProfileModule::TokenizeStreamingProfilePacket(
    PacketSequenceState* sequence_state,
    TraceBlobView* packet,
    ConstBytes streaming_profile_packet) {
  protos::pbzero::StreamingProfilePacket::Decoder decoder(
      streaming_profile_packet.data, streaming_profile_packet.size);

  // We have to resolve the reference timestamp of a StreamingProfilePacket
  // during tokenization. If we did this during parsing instead, the
  // tokenization of a subsequent ThreadDescriptor with a new reference
  // timestamp would cause us to later calculate timestamps based on the wrong
  // reference value during parsing. Since StreamingProfilePackets only need to
  // be sorted correctly with respect to process/thread metadata events (so that
  // pid/tid are resolved correctly during parsing), we forward the packet as a
  // whole through the sorter, using the "root" timestamp of the packet, i.e.
  // the current timestamp of the packet sequence.
  auto packet_ts =
      sequence_state->IncrementAndGetTrackEventTimeNs(/*delta_ns=*/0);
  base::StatusOr<int64_t> trace_ts = context_->clock_tracker->ToTraceTime(
      protos::pbzero::BUILTIN_CLOCK_MONOTONIC, packet_ts);
  if (trace_ts.ok())
    packet_ts = *trace_ts;

  // Increment the sequence's timestamp by all deltas.
  for (auto timestamp_it = decoder.timestamp_delta_us(); timestamp_it;
       ++timestamp_it) {
    sequence_state->IncrementAndGetTrackEventTimeNs(*timestamp_it * 1000);
  }

  context_->sorter->PushTracePacket(
      packet_ts, sequence_state->current_generation(), std::move(*packet));
  return ModuleResult::Handled();
}

void ProfileModule::ParseStreamingProfilePacket(
    int64_t timestamp,
    PacketSequenceStateGeneration* sequence_state,
    ConstBytes streaming_profile_packet) {
  protos::pbzero::StreamingProfilePacket::Decoder packet(
      streaming_profile_packet.data, streaming_profile_packet.size);

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  SequenceStackProfileTracker& sequence_stack_profile_tracker =
      sequence_state->state()->sequence_stack_profile_tracker();
  ProfilePacketInternLookup intern_lookup(sequence_state);

  uint32_t pid = static_cast<uint32_t>(sequence_state->state()->pid());
  uint32_t tid = static_cast<uint32_t>(sequence_state->state()->tid());
  UniqueTid utid = procs->UpdateThread(tid, pid);

  // Iterate through timestamps and callstacks simultaneously.
  auto timestamp_it = packet.timestamp_delta_us();
  for (auto callstack_it = packet.callstack_iid(); callstack_it;
       ++callstack_it, ++timestamp_it) {
    if (!timestamp_it) {
      context_->storage->IncrementStats(stats::stackprofile_parser_error);
      PERFETTO_ELOG(
          "StreamingProfilePacket has less callstack IDs than timestamps!");
      break;
    }

    auto opt_cs_id = sequence_stack_profile_tracker.FindOrInsertCallstack(
        *callstack_it, &intern_lookup);
    if (!opt_cs_id) {
      context_->storage->IncrementStats(stats::stackprofile_parser_error);
      continue;
    }

    // Resolve the delta timestamps based on the packet's root timestamp.
    timestamp += *timestamp_it * 1000;

    tables::CpuProfileStackSampleTable::Row sample_row{
        timestamp, *opt_cs_id, utid, packet.process_priority()};
    storage->mutable_cpu_profile_stack_sample_table()->Insert(sample_row);
  }
}

void ProfileModule::ParsePerfSample(
    int64_t ts,
    PacketSequenceStateGeneration* sequence_state,
    const TracePacket::Decoder& decoder) {
  using PerfSample = protos::pbzero::PerfSample;
  const auto& sample_raw = decoder.perf_sample();
  PerfSample::Decoder sample(sample_raw.data, sample_raw.size);

  uint32_t seq_id = decoder.trusted_packet_sequence_id();
  PerfSampleTracker::SamplingStreamInfo sampling_stream =
      context_->perf_sample_tracker->GetSamplingStreamInfo(
          seq_id, sample.cpu(), sequence_state->GetTracePacketDefaults());

  // Not a sample, but an indication of data loss in the ring buffer shared with
  // the kernel.
  if (sample.kernel_records_lost() > 0) {
    PERFETTO_DCHECK(sample.pid() == 0);

    context_->storage->IncrementIndexedStats(
        stats::perf_cpu_lost_records, static_cast<int>(sample.cpu()),
        static_cast<int64_t>(sample.kernel_records_lost()));
    return;
  }

  // Sample that looked relevant for the tracing session, but had to be skipped.
  // Either we failed to look up the procfs file descriptors necessary for
  // remote stack unwinding (not unexpected in most cases), or the unwind queue
  // was out of capacity (producer lost data on its own).
  if (sample.has_sample_skipped_reason()) {
    context_->storage->IncrementStats(stats::perf_samples_skipped);

    if (sample.sample_skipped_reason() ==
        PerfSample::PROFILER_SKIP_UNWIND_ENQUEUE)
      context_->storage->IncrementStats(stats::perf_samples_skipped_dataloss);

    return;
  }

  // Not a sample, but an event from the producer.
  // TODO(rsavitski): this stat is indexed by the session id, but the older
  // stats (see above) aren't. The indexing is relevant if a trace contains more
  // than one profiling data source. So the older stats should be changed to
  // being indexed as well.
  if (sample.has_producer_event()) {
    PerfSample::ProducerEvent::Decoder producer_event(sample.producer_event());
    if (producer_event.source_stop_reason() ==
        PerfSample::ProducerEvent::PROFILER_STOP_GUARDRAIL) {
      context_->storage->SetIndexedStats(
          stats::perf_guardrail_stop_ts,
          static_cast<int>(sampling_stream.perf_session_id), ts);
    }
    return;
  }

  // Proper sample, populate the |perf_sample| table with everything except the
  // recorded counter values, which go to |counter|.
  context_->event_tracker->PushCounter(
      ts, static_cast<double>(sample.timebase_count()),
      sampling_stream.timebase_track_id);

  SequenceStackProfileTracker& stack_tracker =
      sequence_state->state()->sequence_stack_profile_tracker();
  ProfilePacketInternLookup intern_lookup(sequence_state);
  uint64_t callstack_iid = sample.callstack_iid();
  std::optional<CallsiteId> cs_id =
      stack_tracker.FindOrInsertCallstack(callstack_iid, &intern_lookup);

  // A failed lookup of the interned callstack can mean either:
  // (a) This is a counter-only profile without callstacks. Due to an
  //     implementation quirk, these packets still set callstack_iid
  //     corresponding to a callstack with no frames. To reliably identify this
  //     case (without resorting to config parsing) we further need to rely on
  //     the fact that the implementation (callstack_trie.h) always assigns this
  //     callstack the id "1". Such callstacks should not occur outside of
  //     counter-only profiles, as there should always be at least a synthetic
  //     error frame if the unwinding completely failed.
  // (b) This is a ring-buffer profile where some of the referenced internings
  //     have been overwritten, and the build predates perf_sample_defaults and
  //     SEQ_NEEDS_INCREMENTAL_STATE sequence flag in perf_sample packets.
  //     Such packets should be discarded.
  if (!cs_id && callstack_iid != 1) {
    PERFETTO_DLOG("Discarding perf_sample since callstack_iid [%" PRIu64
                  "] references a missing/partially lost interning according "
                  "to stack_profile_tracker",
                  callstack_iid);
    return;
  }

  UniqueTid utid =
      context_->process_tracker->UpdateThread(sample.tid(), sample.pid());

  using protos::pbzero::Profiling;
  TraceStorage* storage = context_->storage.get();

  auto cpu_mode = static_cast<Profiling::CpuMode>(sample.cpu_mode());
  StringPool::Id cpu_mode_id =
      storage->InternString(ProfilePacketUtils::StringifyCpuMode(cpu_mode));

  std::optional<StringPool::Id> unwind_error_id;
  if (sample.has_unwind_error()) {
    auto unwind_error =
        static_cast<Profiling::StackUnwindError>(sample.unwind_error());
    unwind_error_id = storage->InternString(
        ProfilePacketUtils::StringifyStackUnwindError(unwind_error));
  }
  tables::PerfSampleTable::Row sample_row(ts, utid, sample.cpu(), cpu_mode_id,
                                          cs_id, unwind_error_id,
                                          sampling_stream.perf_session_id);
  context_->storage->mutable_perf_sample_table()->Insert(sample_row);
}

void ProfileModule::ParseProfilePacket(
    int64_t ts,
    PacketSequenceStateGeneration* sequence_state,
    uint32_t seq_id,
    ConstBytes blob) {
  protos::pbzero::ProfilePacket::Decoder packet(blob.data, blob.size);
  context_->heap_profile_tracker->SetProfilePacketIndex(seq_id, packet.index());

  for (auto it = packet.strings(); it; ++it) {
    protos::pbzero::InternedString::Decoder entry(*it);

    const char* str = reinterpret_cast<const char*>(entry.str().data);
    auto str_view = base::StringView(str, entry.str().size);
    sequence_state->state()->sequence_stack_profile_tracker().AddString(
        entry.iid(), str_view);
  }

  for (auto it = packet.mappings(); it; ++it) {
    protos::pbzero::Mapping::Decoder entry(*it);
    SequenceStackProfileTracker::SourceMapping src_mapping =
        ProfilePacketUtils::MakeSourceMapping(entry);
    sequence_state->state()->sequence_stack_profile_tracker().AddMapping(
        entry.iid(), src_mapping);
  }

  for (auto it = packet.frames(); it; ++it) {
    protos::pbzero::Frame::Decoder entry(*it);
    SequenceStackProfileTracker::SourceFrame src_frame =
        ProfilePacketUtils::MakeSourceFrame(entry);
    sequence_state->state()->sequence_stack_profile_tracker().AddFrame(
        entry.iid(), src_frame);
  }

  for (auto it = packet.callstacks(); it; ++it) {
    protos::pbzero::Callstack::Decoder entry(*it);
    SequenceStackProfileTracker::SourceCallstack src_callstack =
        ProfilePacketUtils::MakeSourceCallstack(entry);
    sequence_state->state()->sequence_stack_profile_tracker().AddCallstack(
        entry.iid(), src_callstack);
  }

  for (auto it = packet.process_dumps(); it; ++it) {
    protos::pbzero::ProfilePacket::ProcessHeapSamples::Decoder entry(*it);

    base::StatusOr<int64_t> maybe_timestamp =
        context_->clock_tracker->ToTraceTime(
            protos::pbzero::BUILTIN_CLOCK_MONOTONIC_COARSE,
            static_cast<int64_t>(entry.timestamp()));

    // ToTraceTime() increments the clock_sync_failure error stat in this case.
    if (!maybe_timestamp.ok())
      continue;

    int64_t timestamp = *maybe_timestamp;

    int pid = static_cast<int>(entry.pid());
    context_->storage->SetIndexedStats(stats::heapprofd_last_profile_timestamp,
                                       pid, ts);

    if (entry.disconnected())
      context_->storage->IncrementIndexedStats(
          stats::heapprofd_client_disconnected, pid);
    if (entry.buffer_corrupted())
      context_->storage->IncrementIndexedStats(
          stats::heapprofd_buffer_corrupted, pid);
    if (entry.buffer_overran() ||
        entry.client_error() ==
            protos::pbzero::ProfilePacket::ProcessHeapSamples::
                CLIENT_ERROR_HIT_TIMEOUT) {
      context_->storage->IncrementIndexedStats(stats::heapprofd_buffer_overran,
                                               pid);
    }
    if (entry.client_error()) {
      context_->storage->SetIndexedStats(stats::heapprofd_client_error, pid,
                                         entry.client_error());
    }
    if (entry.rejected_concurrent())
      context_->storage->IncrementIndexedStats(
          stats::heapprofd_rejected_concurrent, pid);
    if (entry.hit_guardrail())
      context_->storage->IncrementIndexedStats(stats::heapprofd_hit_guardrail,
                                               pid);
    if (entry.orig_sampling_interval_bytes()) {
      context_->storage->SetIndexedStats(
          stats::heapprofd_sampling_interval_adjusted, pid,
          static_cast<int64_t>(entry.sampling_interval_bytes()) -
              static_cast<int64_t>(entry.orig_sampling_interval_bytes()));
    }

    protos::pbzero::ProfilePacket::ProcessStats::Decoder stats(entry.stats());
    context_->storage->IncrementIndexedStats(
        stats::heapprofd_unwind_time_us, static_cast<int>(entry.pid()),
        static_cast<int64_t>(stats.total_unwinding_time_us()));
    context_->storage->IncrementIndexedStats(
        stats::heapprofd_unwind_samples, static_cast<int>(entry.pid()),
        static_cast<int64_t>(stats.heap_samples()));
    context_->storage->IncrementIndexedStats(
        stats::heapprofd_client_spinlock_blocked, static_cast<int>(entry.pid()),
        static_cast<int64_t>(stats.client_spinlock_blocked_us()));

    // orig_sampling_interval_bytes was introduced slightly after a bug with
    // self_max_count was fixed in the producer. We use this as a proxy
    // whether or not we are getting this data from a fixed producer or not.
    bool trustworthy_max_count = entry.orig_sampling_interval_bytes() > 0;

    for (auto sample_it = entry.samples(); sample_it; ++sample_it) {
      protos::pbzero::ProfilePacket::HeapSample::Decoder sample(*sample_it);

      HeapProfileTracker::SourceAllocation src_allocation;
      src_allocation.pid = entry.pid();
      if (entry.heap_name().size != 0) {
        src_allocation.heap_name =
            context_->storage->InternString(entry.heap_name());
      } else {
        src_allocation.heap_name = context_->storage->InternString("malloc");
      }
      src_allocation.timestamp = timestamp;
      src_allocation.callstack_id = sample.callstack_id();
      if (sample.has_self_max()) {
        src_allocation.self_allocated = sample.self_max();
        if (trustworthy_max_count)
          src_allocation.alloc_count = sample.self_max_count();
      } else {
        src_allocation.self_allocated = sample.self_allocated();
        src_allocation.self_freed = sample.self_freed();
        src_allocation.alloc_count = sample.alloc_count();
        src_allocation.free_count = sample.free_count();
      }

      context_->heap_profile_tracker->StoreAllocation(seq_id, src_allocation);
    }
  }
  if (!packet.continued()) {
    PERFETTO_CHECK(sequence_state);
    ProfilePacketInternLookup intern_lookup(sequence_state);
    context_->heap_profile_tracker->FinalizeProfile(
        seq_id, &sequence_state->state()->sequence_stack_profile_tracker(),
        &intern_lookup);
  }
}

void ProfileModule::ParseModuleSymbols(ConstBytes blob) {
  protos::pbzero::ModuleSymbols::Decoder module_symbols(blob.data, blob.size);
  StringId build_id;
  // TODO(b/148109467): Remove workaround once all active Chrome versions
  // write raw bytes instead of a string as build_id.
  if (util::IsHexModuleId(module_symbols.build_id())) {
    build_id = context_->storage->InternString(module_symbols.build_id());
  } else {
    build_id = context_->storage->InternString(base::StringView(base::ToHex(
        module_symbols.build_id().data, module_symbols.build_id().size)));
  }

  auto mapping_ids = context_->global_stack_profile_tracker->FindMappingRow(
      context_->storage->InternString(module_symbols.path()), build_id);
  if (mapping_ids.empty()) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_mapping_id);
    return;
  }
  for (auto addr_it = module_symbols.address_symbols(); addr_it; ++addr_it) {
    protos::pbzero::AddressSymbols::Decoder address_symbols(*addr_it);

    uint32_t symbol_set_id = context_->storage->symbol_table().row_count();

    bool has_lines = false;
    // Taking the last (i.e. the least interned) location if there're several.
    ArgsTranslationTable::SourceLocation last_location;
    for (auto line_it = address_symbols.lines(); line_it; ++line_it) {
      protos::pbzero::Line::Decoder line(*line_it);
      context_->storage->mutable_symbol_table()->Insert(
          {symbol_set_id, context_->storage->InternString(line.function_name()),
           context_->storage->InternString(line.source_file_name()),
           line.line_number()});
      last_location = ArgsTranslationTable::SourceLocation{
          line.source_file_name().ToStdString(),
          line.function_name().ToStdString(), line.line_number()};
      has_lines = true;
    }
    if (!has_lines) {
      continue;
    }
    bool frame_found = false;
    for (MappingId mapping_id : mapping_ids) {
      context_->args_translation_table->AddNativeSymbolTranslationRule(
          mapping_id, address_symbols.address(), last_location);
      std::vector<FrameId> frame_ids =
          context_->global_stack_profile_tracker->FindFrameIds(
              mapping_id, address_symbols.address());

      for (const FrameId frame_id : frame_ids) {
        auto* frames = context_->storage->mutable_stack_profile_frame_table();
        uint32_t frame_row = *frames->id().IndexOf(frame_id);
        frames->mutable_symbol_set_id()->Set(frame_row, symbol_set_id);
        frame_found = true;
      }
    }

    if (!frame_found) {
      context_->storage->IncrementStats(stats::stackprofile_invalid_frame_id);
      continue;
    }
  }
}

void ProfileModule::ParseDeobfuscationMapping(int64_t,
                                              PacketSequenceStateGeneration*,
                                              uint32_t /* seq_id */,
                                              ConstBytes blob) {
  DeobfuscationMappingTable deobfuscation_mapping_table;
  protos::pbzero::DeobfuscationMapping::Decoder deobfuscation_mapping(
      blob.data, blob.size);
  if (deobfuscation_mapping.package_name().size == 0)
    return;

  auto opt_package_name_id = context_->storage->string_pool().GetId(
      deobfuscation_mapping.package_name());
  auto opt_memfd_id = context_->storage->string_pool().GetId("memfd");
  if (!opt_package_name_id && !opt_memfd_id)
    return;

  for (auto class_it = deobfuscation_mapping.obfuscated_classes(); class_it;
       ++class_it) {
    protos::pbzero::ObfuscatedClass::Decoder cls(*class_it);
    base::FlatHashMap<StringId, StringId> obfuscated_to_deobfuscated_members;
    for (auto member_it = cls.obfuscated_methods(); member_it; ++member_it) {
      protos::pbzero::ObfuscatedMember::Decoder member(*member_it);
      std::string merged_obfuscated = cls.obfuscated_name().ToStdString() +
                                      "." +
                                      member.obfuscated_name().ToStdString();
      auto merged_obfuscated_id = context_->storage->string_pool().GetId(
          base::StringView(merged_obfuscated));
      if (!merged_obfuscated_id)
        continue;
      std::string merged_deobfuscated =
          FullyQualifiedDeobfuscatedName(cls, member);

      std::vector<tables::StackProfileFrameTable::Id> frames;
      if (opt_package_name_id) {
        const std::vector<tables::StackProfileFrameTable::Id>* pkg_frames =
            context_->global_stack_profile_tracker->JavaFramesForName(
                {*merged_obfuscated_id, *opt_package_name_id});
        if (pkg_frames) {
          frames.insert(frames.end(), pkg_frames->begin(), pkg_frames->end());
        }
      }
      if (opt_memfd_id) {
        const std::vector<tables::StackProfileFrameTable::Id>* memfd_frames =
            context_->global_stack_profile_tracker->JavaFramesForName(
                {*merged_obfuscated_id, *opt_memfd_id});
        if (memfd_frames) {
          frames.insert(frames.end(), memfd_frames->begin(),
                        memfd_frames->end());
        }
      }

      for (tables::StackProfileFrameTable::Id frame_id : frames) {
        auto* frames_tbl =
            context_->storage->mutable_stack_profile_frame_table();
        frames_tbl->mutable_deobfuscated_name()->Set(
            *frames_tbl->id().IndexOf(frame_id),
            context_->storage->InternString(
                base::StringView(merged_deobfuscated)));
      }
      obfuscated_to_deobfuscated_members[context_->storage->InternString(
          member.obfuscated_name())] =
          context_->storage->InternString(member.deobfuscated_name());
    }
    // Members can contain a class name (e.g "ClassA.FunctionF")
    deobfuscation_mapping_table.AddClassTranslation(
        DeobfuscationMappingTable::PackageId{
            deobfuscation_mapping.package_name().ToStdString(),
            deobfuscation_mapping.version_code()},
        context_->storage->InternString(cls.obfuscated_name()),
        context_->storage->InternString(cls.deobfuscated_name()),
        std::move(obfuscated_to_deobfuscated_members));
  }
  context_->args_translation_table->AddDeobfuscationMappingTable(
      std::move(deobfuscation_mapping_table));
}

void ProfileModule::ParseSmapsPacket(int64_t ts, ConstBytes blob) {
  protos::pbzero::SmapsPacket::Decoder sp(blob.data, blob.size);
  auto upid = context_->process_tracker->GetOrCreateProcess(sp.pid());

  for (auto it = sp.entries(); it; ++it) {
    protos::pbzero::SmapsEntry::Decoder e(*it);
    context_->storage->mutable_profiler_smaps_table()->Insert(
        {upid, ts, context_->storage->InternString(e.path()),
         static_cast<int64_t>(e.size_kb()),
         static_cast<int64_t>(e.private_dirty_kb()),
         static_cast<int64_t>(e.swap_kb()),
         context_->storage->InternString(e.file_name()),
         static_cast<int64_t>(e.start_address()),
         static_cast<int64_t>(e.module_timestamp()),
         context_->storage->InternString(e.module_debugid()),
         context_->storage->InternString(e.module_debug_path()),
         static_cast<int32_t>(e.protection_flags()),
         static_cast<int64_t>(e.private_clean_resident_kb()),
         static_cast<int64_t>(e.shared_dirty_resident_kb()),
         static_cast<int64_t>(e.shared_clean_resident_kb()),
         static_cast<int64_t>(e.locked_kb()),
         static_cast<int64_t>(e.proportional_resident_kb())});
  }
}

void ProfileModule::NotifyEndOfFile() {
  for (auto it = context_->storage->stack_profile_mapping_table().IterateRows();
       it; ++it) {
    NullTermStringView path = context_->storage->GetString(it.name());
    NullTermStringView build_id = context_->storage->GetString(it.build_id());

    if (path.StartsWith("/data/local/tmp/") && build_id.empty()) {
      context_->storage->IncrementStats(
          stats::symbolization_tmp_build_id_not_found);
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
