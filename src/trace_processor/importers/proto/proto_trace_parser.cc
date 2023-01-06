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

#include "src/trace_processor/importers/proto/proto_trace_parser.h"

#include <string.h>

#include <cinttypes>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/metatrace_events.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/string_writer.h"
#include "perfetto/ext/base/uuid.h"

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_module.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/track_event_module.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/common/trace_stats.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

ProtoTraceParser::ProtoTraceParser(TraceProcessorContext* context)
    : context_(context),
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

ProtoTraceParser::~ProtoTraceParser() = default;

void ProtoTraceParser::ParseTracePacket(int64_t ts, TracePacketData data) {
  const TraceBlobView& blob = data.packet;
  protos::pbzero::TracePacket::Decoder packet(blob.data(), blob.length());
  // TODO(eseckler): Propagate statuses from modules.
  auto& modules = context_->modules_by_field;
  for (uint32_t field_id = 1; field_id < modules.size(); ++field_id) {
    if (!modules[field_id].empty() && packet.Get(field_id).valid()) {
      for (ProtoImporterModule* global_module :
           context_->modules_for_all_fields) {
        global_module->ParseTracePacketData(packet, ts, data, field_id);
      }
      for (ProtoImporterModule* module : modules[field_id])
        module->ParseTracePacketData(packet, ts, data, field_id);
      return;
    }
  }

  if (packet.has_trace_stats())
    ParseTraceStats(packet.trace_stats());

  if (packet.has_chrome_events()) {
    ParseChromeEvents(ts, packet.chrome_events());
  }

  if (packet.has_perfetto_metatrace()) {
    ParseMetatraceEvent(ts, packet.perfetto_metatrace());
  }

  if (packet.has_trace_config()) {
    // TODO(eseckler): Propagate statuses from modules.
    protos::pbzero::TraceConfig::Decoder config(packet.trace_config());
    for (auto& module : context_->modules) {
      module->ParseTraceConfig(config);
    }
  }
}

void ProtoTraceParser::ParseTrackEvent(int64_t ts, TrackEventData data) {
  const TraceBlobView& blob = data.trace_packet_data.packet;
  protos::pbzero::TracePacket::Decoder packet(blob.data(), blob.length());
  context_->track_module->ParseTrackEventData(packet, ts, data);
  context_->args_tracker->Flush();
}

void ProtoTraceParser::ParseFtraceEvent(uint32_t cpu,
                                        int64_t ts,
                                        TracePacketData data) {
  PERFETTO_DCHECK(context_->ftrace_module);
  context_->ftrace_module->ParseFtraceEventData(cpu, ts, data);

  // TODO(lalitm): maybe move this to the flush method in the trace processor
  // once we have it. This may reduce performance in the ArgsTracker though so
  // needs to be handled carefully.
  context_->args_tracker->Flush();
}

void ProtoTraceParser::ParseInlineSchedSwitch(uint32_t cpu,
                                              int64_t ts,
                                              InlineSchedSwitch data) {
  PERFETTO_DCHECK(context_->ftrace_module);
  context_->ftrace_module->ParseInlineSchedSwitch(cpu, ts, data);

  // TODO(lalitm): maybe move this to the flush method in the trace processor
  // once we have it. This may reduce performance in the ArgsTracker though so
  // needs to be handled carefully.
  context_->args_tracker->Flush();
}

void ProtoTraceParser::ParseInlineSchedWaking(uint32_t cpu,
                                              int64_t ts,
                                              InlineSchedWaking data) {
  PERFETTO_DCHECK(context_->ftrace_module);
  context_->ftrace_module->ParseInlineSchedWaking(cpu, ts, data);

  // TODO(lalitm): maybe move this to the flush method in the trace processor
  // once we have it. This may reduce performance in the ArgsTracker though so
  // needs to be handled carefully.
  context_->args_tracker->Flush();
}

void ProtoTraceParser::ParseTraceStats(ConstBytes blob) {
  protos::pbzero::TraceStats::Decoder evt(blob.data, blob.size);
  auto* storage = context_->storage.get();
  storage->SetStats(stats::traced_producers_connected,
                    static_cast<int64_t>(evt.producers_connected()));
  storage->SetStats(stats::traced_producers_seen,
                    static_cast<int64_t>(evt.producers_seen()));
  storage->SetStats(stats::traced_data_sources_registered,
                    static_cast<int64_t>(evt.data_sources_registered()));
  storage->SetStats(stats::traced_data_sources_seen,
                    static_cast<int64_t>(evt.data_sources_seen()));
  storage->SetStats(stats::traced_tracing_sessions,
                    static_cast<int64_t>(evt.tracing_sessions()));
  storage->SetStats(stats::traced_total_buffers,
                    static_cast<int64_t>(evt.total_buffers()));
  storage->SetStats(stats::traced_chunks_discarded,
                    static_cast<int64_t>(evt.chunks_discarded()));
  storage->SetStats(stats::traced_patches_discarded,
                    static_cast<int64_t>(evt.patches_discarded()));
  storage->SetStats(stats::traced_flushes_requested,
                    static_cast<int64_t>(evt.flushes_requested()));
  storage->SetStats(stats::traced_flushes_succeeded,
                    static_cast<int64_t>(evt.flushes_succeeded()));
  storage->SetStats(stats::traced_flushes_failed,
                    static_cast<int64_t>(evt.flushes_failed()));
  switch (evt.final_flush_outcome()) {
    case protos::pbzero::TraceStats::FINAL_FLUSH_SUCCEEDED:
      storage->IncrementStats(stats::traced_final_flush_succeeded, 1);
      break;
    case protos::pbzero::TraceStats::FINAL_FLUSH_FAILED:
      storage->IncrementStats(stats::traced_final_flush_failed, 1);
      break;
    case protos::pbzero::TraceStats::FINAL_FLUSH_UNSPECIFIED:
      break;
  }

  int buf_num = 0;
  for (auto it = evt.buffer_stats(); it; ++it, ++buf_num) {
    protos::pbzero::TraceStats::BufferStats::Decoder buf(*it);
    storage->SetIndexedStats(stats::traced_buf_buffer_size, buf_num,
                             static_cast<int64_t>(buf.buffer_size()));
    storage->SetIndexedStats(stats::traced_buf_bytes_written, buf_num,
                             static_cast<int64_t>(buf.bytes_written()));
    storage->SetIndexedStats(stats::traced_buf_bytes_overwritten, buf_num,
                             static_cast<int64_t>(buf.bytes_overwritten()));
    storage->SetIndexedStats(stats::traced_buf_bytes_read, buf_num,
                             static_cast<int64_t>(buf.bytes_read()));
    storage->SetIndexedStats(stats::traced_buf_padding_bytes_written, buf_num,
                             static_cast<int64_t>(buf.padding_bytes_written()));
    storage->SetIndexedStats(stats::traced_buf_padding_bytes_cleared, buf_num,
                             static_cast<int64_t>(buf.padding_bytes_cleared()));
    storage->SetIndexedStats(stats::traced_buf_chunks_written, buf_num,
                             static_cast<int64_t>(buf.chunks_written()));
    storage->SetIndexedStats(stats::traced_buf_chunks_rewritten, buf_num,
                             static_cast<int64_t>(buf.chunks_rewritten()));
    storage->SetIndexedStats(stats::traced_buf_chunks_overwritten, buf_num,
                             static_cast<int64_t>(buf.chunks_overwritten()));
    storage->SetIndexedStats(stats::traced_buf_chunks_discarded, buf_num,
                             static_cast<int64_t>(buf.chunks_discarded()));
    storage->SetIndexedStats(stats::traced_buf_chunks_read, buf_num,
                             static_cast<int64_t>(buf.chunks_read()));
    storage->SetIndexedStats(
        stats::traced_buf_chunks_committed_out_of_order, buf_num,
        static_cast<int64_t>(buf.chunks_committed_out_of_order()));
    storage->SetIndexedStats(stats::traced_buf_write_wrap_count, buf_num,
                             static_cast<int64_t>(buf.write_wrap_count()));
    storage->SetIndexedStats(stats::traced_buf_patches_succeeded, buf_num,
                             static_cast<int64_t>(buf.patches_succeeded()));
    storage->SetIndexedStats(stats::traced_buf_patches_failed, buf_num,
                             static_cast<int64_t>(buf.patches_failed()));
    storage->SetIndexedStats(stats::traced_buf_readaheads_succeeded, buf_num,
                             static_cast<int64_t>(buf.readaheads_succeeded()));
    storage->SetIndexedStats(stats::traced_buf_readaheads_failed, buf_num,
                             static_cast<int64_t>(buf.readaheads_failed()));
    storage->SetIndexedStats(stats::traced_buf_abi_violations, buf_num,
                             static_cast<int64_t>(buf.abi_violations()));
    storage->SetIndexedStats(
        stats::traced_buf_trace_writer_packet_loss, buf_num,
        static_cast<int64_t>(buf.trace_writer_packet_loss()));
  }
}

void ProtoTraceParser::ParseChromeEvents(int64_t ts, ConstBytes blob) {
  TraceStorage* storage = context_->storage.get();
  protos::pbzero::ChromeEventBundle::Decoder bundle(blob.data, blob.size);
  ArgsTracker args(context_);
  if (bundle.has_metadata()) {
    RawId id = storage->mutable_raw_table()
                   ->Insert({ts, raw_chrome_metadata_event_id_, 0, 0})
                   .id;
    auto inserter = args.AddArgsTo(id);

    uint32_t bundle_index =
        context_->metadata_tracker->IncrementChromeMetadataBundleCount();

    // The legacy untyped metadata is proxied via a special event in the raw
    // table to JSON export.
    for (auto it = bundle.metadata(); it; ++it) {
      protos::pbzero::ChromeMetadata::Decoder metadata(*it);
      Variadic value;
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
        context_->storage->IncrementStats(stats::empty_chrome_metadata);
        continue;
      }

      StringId name_id = storage->InternString(metadata.name());
      args.AddArgsTo(id).AddArg(name_id, value);

      char buffer[2048];
      base::StringWriter writer(buffer, sizeof(buffer));
      writer.AppendString("cr-");
      // If we have data from multiple Chrome instances, append a suffix
      // to differentiate them.
      if (bundle_index > 1) {
        writer.AppendUnsignedInt(bundle_index);
        writer.AppendChar('-');
      }
      writer.AppendString(metadata.name());

      auto metadata_id = storage->InternString(writer.GetStringView());
      context_->metadata_tracker->SetDynamicMetadata(metadata_id, value);
    }
  }

  if (bundle.has_legacy_ftrace_output()) {
    RawId id =
        storage->mutable_raw_table()
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
      RawId id =
          storage->mutable_raw_table()
              ->Insert({ts, raw_chrome_legacy_user_trace_event_id_, 0, 0})
              .id;
      Variadic value =
          Variadic::String(storage->InternString(legacy_trace.data()));
      args.AddArgsTo(id).AddArg(data_name_id_, value);
    }
  }
}

void ProtoTraceParser::ParseMetatraceEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::PerfettoMetatrace::Decoder event(blob.data, blob.size);
  auto utid = context_->process_tracker->GetOrCreateThread(event.thread_id());

  StringId cat_id = metatrace_id_;
  StringId name_id = kNullStringId;

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

    // We stable sort insted of sorting here to avoid changing the order of the
    // args in arrays.
    std::stable_sort(interned.begin(), interned.end(),
                     [](const Arg& a, const Arg& b) {
                       return a.first.raw_id() < b.second.raw_id();
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
        base::StringView key_str = context_->storage->GetString(key);
        if (key_str.size() >= sizeof(buffer) - kMaxIndexSize) {
          PERFETTO_DLOG("Ignoring arg with unreasonbly large size");
          continue;
        }

        base::StringWriter writer(buffer, sizeof(buffer));
        writer.AppendString(key_str);
        writer.AppendChar('[');
        writer.AppendUnsignedInt(current_idx);
        writer.AppendChar(']');

        StringId new_key =
            context_->storage->InternString(writer.GetStringView());
        inserter->AddArg(key, new_key, Variadic::String(it->second));

        current_idx = key == next_key ? current_idx + 1 : 0;
      }
    }
  };

  if (event.has_event_id() || event.has_event_name() ||
      event.has_event_name_iid()) {
    if (event.has_event_id()) {
      auto eid = event.event_id();
      if (eid < metatrace::EVENTS_MAX) {
        name_id = context_->storage->InternString(metatrace::kEventNames[eid]);
      } else {
        base::StackString<64> fallback("Event %d", eid);
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
    if (event.has_counter_id()) {
      auto cid = event.counter_id();
      if (cid < metatrace::COUNTERS_MAX) {
        name_id =
            context_->storage->InternString(metatrace::kCounterNames[cid]);
      } else {
        base::StackString<64> fallback("Counter %d", cid);
        name_id = context_->storage->InternString(fallback.string_view());
      }
    } else {
      name_id = context_->storage->InternString(event.counter_name());
    }
    TrackId track =
        context_->track_tracker->InternThreadCounterTrack(name_id, utid);
    auto opt_id =
        context_->event_tracker->PushCounter(ts, event.counter_value(), track);
    if (opt_id) {
      auto inserter = context_->args_tracker->AddArgsTo(*opt_id);
      args_fn(&inserter);
    }
  }

  if (event.has_overruns())
    context_->storage->IncrementStats(stats::metatrace_overruns);
}

StringId ProtoTraceParser::GetMetatraceInternedString(uint64_t iid) {
  StringId* maybe_id = metatrace_interned_strings_.Find(iid);
  if (!maybe_id)
    return missing_metatrace_interned_string_id_;
  return *maybe_id;
}

}  // namespace trace_processor
}  // namespace perfetto
