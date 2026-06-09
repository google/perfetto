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

#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/metatrace_events.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/common/virtual_memory_mapping.h"
#include "src/trace_processor/importers/etw/etw_module.h"
#include "src/trace_processor/importers/ftrace/ftrace_module.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_module.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/profiling/art_process_metadata.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

ProtoTraceParserImpl::ProtoTraceParserImpl(
    TraceProcessorContext* context,
    ProtoImporterModuleContext* module_context)
    : context_(context),
      module_context_(module_context),
      metatrace_id_(context->storage->InternString("metatrace")),
      data_name_id_(context->storage->InternString("data")),
      raw_chrome_metadata_event_id_(
          context->storage->InternString("chrome_event.metadata")),
      raw_chrome_legacy_system_trace_event_id_(
          context->storage->InternString("chrome_event.legacy_system_trace")),
      raw_chrome_legacy_user_trace_event_id_(
          context->storage->InternString("chrome_event.legacy_user_trace")),
      missing_metatrace_interned_string_id_(
          context->storage->InternString("MISSING STRING")) {}

ProtoTraceParserImpl::~ProtoTraceParserImpl() = default;

void ProtoTraceParserImpl::ParseTracePacket(int64_t ts, TracePacketData data) {
  const TraceBlobView& blob = data.packet;
  protos::pbzero::TracePacket::Decoder packet(blob.data(), blob.length());
  // TODO(eseckler): Propagate statuses from modules.
  auto& modules = module_context_->modules_by_field;
  // GetExtensionSlowly() (not Get()) so modules registered for out-of-tree
  // extension field ids in the `extensions 1000 to 1999` range are dispatched:
  // Get() can't see fields beyond the highest in-tree field id. It fast-paths
  // in-tree ids, and the scan only runs for the (rare) registered high ids.
  for (uint32_t field_id = 1; field_id < modules.size(); ++field_id) {
    if (!modules[field_id].empty() &&
        packet.GetExtensionSlowly(field_id).valid()) {
      for (ProtoImporterModule* module : modules[field_id])
        module->ParseTracePacketData(packet, ts, data, field_id);
      return;
    }
  }

  if (packet.has_chrome_events()) {
    ParseChromeEvents(ts, packet.chrome_events());
  }

  if (packet.has_perfetto_metatrace()) {
    ParseMetatraceEvent(ts, packet.perfetto_metatrace());
  }

  if (packet.has_trace_config()) {
    // TODO(eseckler): Propagate statuses from modules.
    protos::pbzero::TraceConfig::Decoder config(packet.trace_config());
    for (auto& module : module_context_->modules) {
      module->ParseTraceConfig(config);
    }
  }

  if (packet.has_art_process_metadata()) {
    ParseArtProcessMetadata(ts, packet.art_process_metadata());
  }
}

void ProtoTraceParserImpl::ParseTrackEvent(int64_t ts, TrackEventData data) {
  const TraceBlobView& blob = data.trace_packet_data.packet;
  protos::pbzero::TracePacket::Decoder packet(blob.data(), blob.length());
  module_context_->track_module->ParseTrackEventData(packet, ts, data);
}

void ProtoTraceParserImpl::ParseEtwEvent(uint32_t cpu,
                                         int64_t ts,
                                         TracePacketData data) {
  PERFETTO_DCHECK(module_context_->etw_module);
  module_context_->etw_module->ParseEtwEventData(cpu, ts, data);
}

void ProtoTraceParserImpl::ParseFtraceEvent(uint32_t cpu,
                                            int64_t ts,
                                            FtraceData data) {
  PERFETTO_DCHECK(module_context_->ftrace_module);
  module_context_->ftrace_module->ParseFtraceEventData(cpu, ts, data);
}

void ProtoTraceParserImpl::ParseInlineSchedSwitch(uint32_t cpu,
                                                  int64_t ts,
                                                  InlineSchedSwitch data) {
  PERFETTO_DCHECK(module_context_->ftrace_module);
  module_context_->ftrace_module->ParseInlineSchedSwitch(cpu, ts, data);
}

void ProtoTraceParserImpl::ParseInlineSchedWaking(uint32_t cpu,
                                                  int64_t ts,
                                                  InlineSchedWaking data) {
  PERFETTO_DCHECK(module_context_->ftrace_module);
  module_context_->ftrace_module->ParseInlineSchedWaking(cpu, ts, data);
}

void ProtoTraceParserImpl::ParseChromeEvents(int64_t ts, ConstBytes blob) {
  TraceStorage* storage = context_->storage.get();
  protos::pbzero::ChromeEventBundle::Decoder bundle(blob);
  ArgsTracker args(context_);
  if (bundle.has_metadata()) {
    tables::ChromeRawTable::Id id =
        storage->mutable_chrome_raw_table()
            ->Insert({ts, raw_chrome_metadata_event_id_, 0, 0})
            .id;
    auto inserter = args.AddArgsTo(id);

    uint32_t bundle_index =
        context_->metadata_tracker->IncrementChromeMetadataBundleCount();

    // The legacy untyped metadata is proxied via a special event in the raw
    // table to JSON export.
    for (auto it = bundle.metadata(); it; ++it) {
      protos::pbzero::ChromeMetadata::Decoder metadata(*it);
      Variadic value = Variadic::Null();
      if (metadata.has_string_value()) {
        value =
            Variadic::String(storage->InternString(metadata.string_value()));
      } else if (metadata.has_int_value()) {
        value = Variadic::Integer(metadata.int_value());
      } else if (metadata.has_bool_value()) {
        value = Variadic::Integer(metadata.bool_value());
      } else if (metadata.has_json_value()) {
        value = Variadic::Json(storage->InternString(metadata.json_value()));
      } else {
        context_->stats_tracker->IncrementStats(stats::empty_chrome_metadata);
        continue;
      }

      StringId name_id = storage->InternString(metadata.name());
      args.AddArgsTo(id).AddArg(name_id, value);

      // metadata.name() comes from the trace and is untrusted/unbounded,
      // so we build the key on the heap rather than a fixed stack buffer.
      std::string key = "cr-";
      // If we have data from multiple Chrome instances, append a suffix
      // to differentiate them.
      if (bundle_index > 1) {
        key += std::to_string(bundle_index);
        key += '-';
      }
      key.append(metadata.name().data, metadata.name().size);

      auto metadata_id = storage->InternString(base::StringView(key));
      context_->metadata_tracker->SetDynamicMetadata(metadata_id, value);
    }
  }

  if (bundle.has_legacy_ftrace_output()) {
    tables::ChromeRawTable::Id id =
        storage->mutable_chrome_raw_table()
            ->Insert({ts, raw_chrome_legacy_system_trace_event_id_, 0, 0})
            .id;

    std::string data;
    for (auto it = bundle.legacy_ftrace_output(); it; ++it) {
      data += (*it).ToStdString();
    }
    Variadic value =
        Variadic::String(storage->InternString(base::StringView(data)));
    args.AddArgsTo(id).AddArg(data_name_id_, value);
  }

  if (bundle.has_legacy_json_trace()) {
    for (auto it = bundle.legacy_json_trace(); it; ++it) {
      protos::pbzero::ChromeLegacyJsonTrace::Decoder legacy_trace(*it);
      if (legacy_trace.type() !=
          protos::pbzero::ChromeLegacyJsonTrace::USER_TRACE) {
        continue;
      }
      tables::ChromeRawTable::Id id =
          storage->mutable_chrome_raw_table()
              ->Insert({ts, raw_chrome_legacy_user_trace_event_id_, 0, 0})
              .id;
      Variadic value =
          Variadic::String(storage->InternString(legacy_trace.data()));
      args.AddArgsTo(id).AddArg(data_name_id_, value);
    }
  }
}

void ProtoTraceParserImpl::ParseMetatraceEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::PerfettoMetatrace::Decoder event(blob);
  auto utid = context_->process_tracker->GetOrCreateThread(event.thread_id());

  StringId cat_id = metatrace_id_;
  for (auto it = event.interned_strings(); it; ++it) {
    protos::pbzero::PerfettoMetatrace::InternedString::Decoder interned_string(
        it->data(), it->size());
    metatrace_interned_strings_.Insert(
        interned_string.iid(),
        context_->storage->InternString(interned_string.value()));
  }

  // This function inserts the args from the proto into the args table.
  // Args inserted with the same key multiple times are treated as an array:
  // this function correctly creates the key and flat key for each arg array.
  auto args_fn = [this, &event](ArgsTracker::BoundInserter* inserter) {
    using Arg = std::pair<StringId, StringId>;

    // First, get a list of all the args so we can group them by key.
    std::vector<Arg> interned;
    for (auto it = event.args(); it; ++it) {
      protos::pbzero::PerfettoMetatrace::Arg::Decoder arg_proto(*it);
      StringId key;
      if (arg_proto.has_key_iid()) {
        key = GetMetatraceInternedString(arg_proto.key_iid());
      } else {
        key = context_->storage->InternString(arg_proto.key());
      }
      StringId value;
      if (arg_proto.has_value_iid()) {
        value = GetMetatraceInternedString(arg_proto.value_iid());
      } else {
        value = context_->storage->InternString(arg_proto.value());
      }
      interned.emplace_back(key, value);
    }

    // We stable sort instead of sorting here to avoid changing the order of the
    // args in arrays.
    std::stable_sort(interned.begin(), interned.end(),
                     [](const Arg& a, const Arg& b) {
                       return a.first.raw_id() < b.first.raw_id();
                     });

    // Compute the correct key for each arg, possibly adding an index to
    // the end of the key if needed.
    char buffer[2048];
    uint32_t current_idx = 0;
    for (auto it = interned.begin(); it != interned.end(); ++it) {
      auto next = it + 1;
      StringId key = it->first;
      StringId next_key = next == interned.end() ? kNullStringId : next->first;

      if (key != next_key && current_idx == 0) {
        inserter->AddArg(key, Variadic::String(it->second));
      } else {
        constexpr size_t kMaxIndexSize = 20;
        NullTermStringView key_str = context_->storage->GetString(key);
        if (key_str.size() >= sizeof(buffer) - kMaxIndexSize) {
          PERFETTO_DLOG("Ignoring arg with unreasonbly large size");
          continue;
        }

        base::StackString<2048> array_key("%s[%u]", key_str.c_str(),
                                          current_idx);
        StringId new_key =
            context_->storage->InternString(array_key.string_view());
        inserter->AddArg(key, new_key, Variadic::String(it->second));

        current_idx = key == next_key ? current_idx + 1 : 0;
      }
    }
  };

  if (event.has_event_id() || event.has_event_name() ||
      event.has_event_name_iid()) {
    StringId name_id;
    if (event.has_event_id()) {
      auto eid = event.event_id();
      if (eid < metatrace::EVENTS_MAX) {
        name_id = context_->storage->InternString(metatrace::kEventNames[eid]);
      } else {
        base::StackString<64> fallback("Event %u", eid);
        name_id = context_->storage->InternString(fallback.string_view());
      }
    } else if (event.has_event_name_iid()) {
      name_id = GetMetatraceInternedString(event.event_name_iid());
    } else {
      name_id = context_->storage->InternString(event.event_name());
    }
    TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
    context_->slice_tracker->Scoped(
        ts, track_id, cat_id, name_id,
        static_cast<int64_t>(event.event_duration_ns()), args_fn);
  } else if (event.has_counter_id() || event.has_counter_name()) {
    static constexpr auto kBlueprint = tracks::CounterBlueprint(
        "metatrace_counter", tracks::UnknownUnitBlueprint(),
        tracks::DimensionBlueprints(
            tracks::kThreadDimensionBlueprint,
            tracks::StringDimensionBlueprint("counter_name")),
        tracks::DynamicNameBlueprint());
    TrackId track;
    if (event.has_counter_id()) {
      auto cid = event.counter_id();
      StringId name_id;
      if (cid < metatrace::COUNTERS_MAX) {
        name_id =
            context_->storage->InternString(metatrace::kCounterNames[cid]);
      } else {
        base::StackString<64> fallback("Counter %u", cid);
        name_id = context_->storage->InternString(fallback.string_view());
      }
      track = context_->track_tracker->InternTrack(
          kBlueprint,
          tracks::Dimensions(utid, context_->storage->GetString(name_id)),
          tracks::DynamicName(name_id));
    } else {
      track = context_->track_tracker->InternTrack(
          kBlueprint, tracks::Dimensions(utid, event.counter_name()),
          tracks::DynamicName(
              context_->storage->InternString(event.counter_name())));
    }
    auto opt_id =
        context_->event_tracker->PushCounter(ts, event.counter_value(), track);
    if (opt_id) {
      ArgsTracker args_tracker(context_);
      auto inserter = args_tracker.AddArgsTo(*opt_id);
      args_fn(&inserter);
    }
  }

  if (event.has_overruns())
    context_->stats_tracker->IncrementStats(stats::metatrace_overruns);
}

namespace {

UniquePid GetOrCreateProcess(TraceProcessorContext* context,
                             uint32_t pid,
                             std::optional<base::StringView> process_name,
                             std::optional<uint32_t> uid) {
  context->process_tracker->UpdateThread(pid, pid);
  UniquePid upid = context->process_tracker->GetOrCreateProcess(pid);

  if (process_name.has_value()) {
    StringId process_name_id = context->storage->InternString(*process_name);
    context->process_tracker->UpdateProcessName(
        upid, process_name_id, ProcessNamePriority::kTrackDescriptor);
  }
  if (uid.has_value()) {
    context->process_tracker->SetProcessUid(upid, *uid);
  }
  return upid;
}

void UpdatePackageList(TraceProcessorContext* context,
                       base::StringView package_name,
                       int64_t uid) {
  StringId package_name_id = context->storage->InternString(package_name);

  bool found = false;
  const auto& package_list = context->storage->package_list_table();
  for (auto it = package_list.IterateRows(); it; ++it) {
    if (it.package_name() == package_name_id && it.uid() == uid) {
      found = true;
      break;
    }
  }
  if (!found) {
    context->storage->mutable_package_list_table()->Insert(
        {package_name_id, uid, /*debuggable*/ false,
         /*profileable_from_shell*/ false, /*version_code*/ 0});
  }
}

tables::HeapGraphTable::Id InsertHeapGraph(TraceProcessorContext* context,
                                           int64_t ts,
                                           UniquePid upid) {
  tables::HeapGraphTable::Row heap_graph_row;
  heap_graph_row.ts = ts;
  heap_graph_row.upid = upid;
  heap_graph_row.dump_reason = context->storage->InternString("OOME");

  return context->storage->mutable_heap_graph_table()
      ->Insert(heap_graph_row)
      .id;
}

void InsertOomeDetails(TraceProcessorContext* context,
                       tables::HeapGraphTable::Id heap_graph_id,
                       int64_t byte_count,
                       int64_t total_bytes_free,
                       int64_t free_bytes_until_oom,
                       std::optional<base::StringView> error_msg) {
  tables::HeapGraphJavaOomeDetailsTable::Row oome_details_row;
  oome_details_row.heap_graph_id = heap_graph_id;
  oome_details_row.byte_count = byte_count;
  oome_details_row.total_bytes_free = total_bytes_free;
  oome_details_row.free_bytes_until_oom = free_bytes_until_oom;

  if (error_msg.has_value()) {
    oome_details_row.error_msg = context->storage->InternString(*error_msg);
  }

  context->storage->mutable_heap_graph_java_oome_details_table()->Insert(
      oome_details_row);
}

void InsertOomeHeapGraphCallsite(TraceProcessorContext* context,
                                 tables::HeapGraphTable::Id heap_graph_id,
                                 uint32_t pid,
                                 protozero::ConstBytes stack_bytes,
                                 DummyMemoryMapping*& art_oome_mapping) {
  protos::pbzero::JavaStack::Decoder stack_decoder(stack_bytes.data,
                                                   stack_bytes.size);

  std::vector<::protozero::ConstBytes> raw_frames;
  for (auto it = stack_decoder.frames(); it; ++it) {
    raw_frames.push_back(*it);
  }

  std::optional<CallsiteId> current_callsite_id = std::nullopt;
  uint32_t depth = 0;

  if (!art_oome_mapping) {
    art_oome_mapping =
        &context->mapping_tracker->CreateDummyMapping("art_oome");
  }

  for (auto it = raw_frames.rbegin(); it != raw_frames.rend(); ++it) {
    protos::pbzero::JavaFrame::Decoder frame_decoder(*it);
    if (!frame_decoder.has_method_name()) {
      continue;
    }

    base::StringView method_name(
        reinterpret_cast<const char*>(frame_decoder.method_name().data),
        frame_decoder.method_name().size);

    std::optional<base::StringView> source_file = std::nullopt;
    if (frame_decoder.has_source_file()) {
      source_file = base::StringView(
          reinterpret_cast<const char*>(frame_decoder.source_file().data),
          frame_decoder.source_file().size);
    }

    std::optional<uint32_t> line_number = std::nullopt;
    if (frame_decoder.has_line_number()) {
      line_number = static_cast<uint32_t>(frame_decoder.line_number());
    }

    FrameId frame_id = art_oome_mapping->InternDummyFrame(
        method_name, source_file, line_number);

    current_callsite_id = context->stack_profile_tracker->InternCallsite(
        current_callsite_id, frame_id, depth++);
  }

  tables::HeapGraphThreadCallsiteTable::Row callsite_row;
  callsite_row.heap_graph_id = heap_graph_id;
  UniqueTid utid = context->process_tracker->UpdateThread(pid, pid);
  callsite_row.utid = utid;
  callsite_row.callsite_id = current_callsite_id;

  context->storage->mutable_heap_graph_thread_callsite_table()->Insert(
      callsite_row);
}

}  // namespace

void ProtoTraceParserImpl::ParseArtProcessMetadata(int64_t ts,
                                                   ConstBytes blob) {
  protos::pbzero::ArtProcessMetadata::Decoder decoder(blob.data, blob.size);
  if (!decoder.has_oom_allocation_size()) {
    return;
  }

  uint32_t pid = static_cast<uint32_t>(decoder.pid());
  std::optional<base::StringView> process_name;
  if (decoder.has_process_name()) {
    process_name = decoder.process_name();
  }
  std::optional<uint32_t> uid;
  if (decoder.has_uid()) {
    uid = static_cast<uint32_t>(decoder.uid());
  }

  UniquePid upid = GetOrCreateProcess(context_, pid, process_name, uid);

  if (decoder.has_package_name() && decoder.has_uid()) {
    UpdatePackageList(context_, decoder.package_name(), decoder.uid());
  }

  tables::HeapGraphTable::Id heap_graph_id =
      InsertHeapGraph(context_, ts, upid);

  std::optional<base::StringView> error_msg;
  if (decoder.has_oom_error_msg()) {
    error_msg = decoder.oom_error_msg();
  }

  InsertOomeDetails(context_, heap_graph_id,
                    static_cast<int64_t>(decoder.oom_allocation_size()),
                    static_cast<int64_t>(decoder.oom_total_bytes_free()),
                    static_cast<int64_t>(decoder.oom_free_bytes_until_oom()),
                    error_msg);

  if (decoder.has_oom_thread_java_stack()) {
    InsertOomeHeapGraphCallsite(context_, heap_graph_id, pid,
                                decoder.oom_thread_java_stack(),
                                art_oome_mapping_);
  }
}

StringId ProtoTraceParserImpl::GetMetatraceInternedString(uint64_t iid) {
  StringId* maybe_id = metatrace_interned_strings_.Find(iid);
  if (!maybe_id)
    return missing_metatrace_interned_string_id_;
  return *maybe_id;
}

}  // namespace perfetto::trace_processor
