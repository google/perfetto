/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/network_trace_module.h"

#include "perfetto/ext/base/string_writer.h"
#include "protos/perfetto/trace/android/network_trace.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/tcp_state.h"

namespace perfetto {
namespace trace_processor {
namespace {
// Convert the bitmask into a string where '.' indicates an unset bit
// and each bit gets a unique letter if set. The letters correspond to
// the bitfields in tcphdr (fin, syn, rst, etc).
base::StackString<12> GetTcpFlagMask(uint32_t tcp_flags) {
  static constexpr char kBitNames[] = "fsrpauec";
  static constexpr int kBitCount = 8;

  char flags[kBitCount + 1] = {'\0'};
  for (int f = 0; f < kBitCount; f++) {
    flags[f] = (tcp_flags & (1 << f)) ? kBitNames[f] : '.';
  }

  return base::StackString<12>("%s", flags);
}
}  // namespace

using ::perfetto::protos::pbzero::NetworkPacketBundle;
using ::perfetto::protos::pbzero::NetworkPacketEvent;
using ::perfetto::protos::pbzero::TracePacket;
using ::perfetto::protos::pbzero::TrafficDirection;
using ::protozero::ConstBytes;

NetworkTraceModule::NetworkTraceModule(TraceProcessorContext* context)
    : context_(context),
      net_arg_length_(context->storage->InternString("packet_length")),
      net_arg_ip_proto_(context->storage->InternString("packet_transport")),
      net_arg_tcp_flags_(context->storage->InternString("packet_tcp_flags")),
      net_arg_tag_(context->storage->InternString("socket_tag")),
      net_arg_local_port_(context->storage->InternString("local_port")),
      net_arg_remote_port_(context->storage->InternString("remote_port")),
      net_ipproto_tcp_(context->storage->InternString("IPPROTO_TCP")),
      net_ipproto_udp_(context->storage->InternString("IPPROTO_UDP")) {
  RegisterForField(TracePacket::kNetworkPacketFieldNumber, context);
  RegisterForField(TracePacket::kNetworkPacketBundleFieldNumber, context);
}

ModuleResult NetworkTraceModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView*,
    int64_t ts,
    PacketSequenceState* state,
    uint32_t field_id) {
  if (field_id != TracePacket::kNetworkPacketBundleFieldNumber) {
    return ModuleResult::Ignored();
  }

  auto seq_state = state->current_generation();
  NetworkPacketBundle::Decoder evt(decoder.network_packet_bundle());

  ConstBytes context = evt.ctx();
  if (evt.has_iid()) {
    auto* interned = seq_state->LookupInternedMessage<
        protos::pbzero::InternedData::kPacketContextFieldNumber,
        protos::pbzero::NetworkPacketContext>(evt.iid());
    if (!interned) {
      context_->storage->IncrementStats(stats::network_trace_intern_errors);
    } else {
      context = interned->ctx();
    }
  }

  if (evt.has_total_length()) {
    // Forward the bundle with (possibly de-interned) context.
    packet_buffer_->set_timestamp(static_cast<uint64_t>(ts));
    auto* event = packet_buffer_->set_network_packet_bundle();
    event->set_ctx()->AppendRawProtoBytes(context.data, context.size);
    event->set_total_length(evt.total_length());
    event->set_total_packets(evt.total_packets());
    event->set_total_duration(evt.total_duration());
    PushPacketBufferForSort(ts, state);
  } else {
    // Push a NetworkPacketEvent for each packet in the packed arrays.
    bool parse_error = false;
    auto length_iter = evt.packet_lengths(&parse_error);
    auto timestamp_iter = evt.packet_timestamps(&parse_error);
    if (parse_error) {
      context_->storage->IncrementStats(stats::network_trace_parse_errors);
      return ModuleResult::Handled();
    }

    for (; timestamp_iter && length_iter; ++timestamp_iter, ++length_iter) {
      int64_t real_ts = ts + static_cast<int64_t>(*timestamp_iter);
      packet_buffer_->set_timestamp(static_cast<uint64_t>(real_ts));
      auto* event = packet_buffer_->set_network_packet();
      event->AppendRawProtoBytes(context.data, context.size);
      event->set_length(*length_iter);
      PushPacketBufferForSort(real_ts, state);
    }
  }

  return ModuleResult::Handled();
}

void NetworkTraceModule::ParseTracePacketData(
    const TracePacket::Decoder& decoder,
    int64_t ts,
    const TracePacketData&,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kNetworkPacketFieldNumber:
      ParseNetworkPacketEvent(ts, decoder.network_packet());
      return;
  }
}

void NetworkTraceModule::ParseNetworkPacketEvent(int64_t ts, ConstBytes blob) {
  using protos::pbzero::NetworkPacketEvent;
  using protos::pbzero::TrafficDirection;
  NetworkPacketEvent::Decoder evt(blob);

  // Tracks are per interface and per direction.
  const char* track_suffix =
      evt.direction() == TrafficDirection::DIR_INGRESS  ? " Received"
      : evt.direction() == TrafficDirection::DIR_EGRESS ? " Transmitted"
                                                        : " DIR_UNKNOWN";

  base::StackString<64> name("%.*s%s", static_cast<int>(evt.interface().size),
                             evt.interface().data, track_suffix);
  StringId name_id = context_->storage->InternString(name.string_view());

  // Event titles are the package name, if available.
  StringId title_id = kNullStringId;
  if (evt.uid() > 0) {
    const auto& package_list = context_->storage->package_list_table();
    base::Optional<uint32_t> pkg_row = package_list.uid().IndexOf(evt.uid());
    if (pkg_row) {
      title_id = package_list.package_name()[*pkg_row];
    }
  }

  // If the above fails, fall back to the uid.
  if (title_id == kNullStringId) {
    base::StackString<32> title_str("uid=%" PRIu32, evt.uid());
    title_id = context_->storage->InternString(title_str.string_view());
  }

  TrackId track_id = context_->async_track_set_tracker->Scoped(
      context_->async_track_set_tracker->InternGlobalTrackSet(name_id), ts, 0);

  context_->slice_tracker->Scoped(
      ts, track_id, name_id, title_id, 0, [&](ArgsTracker::BoundInserter* i) {
        i->AddArg(net_arg_length_, Variadic::Integer(evt.length()));

        StringId ip_proto;
        if (evt.ip_proto() == kIpprotoTcp) {
          ip_proto = net_ipproto_tcp_;
        } else if (evt.ip_proto() == kIpprotoUdp) {
          ip_proto = net_ipproto_udp_;
        } else {
          base::StackString<32> proto("IPPROTO (%d)", evt.ip_proto());
          ip_proto = context_->storage->InternString(proto.string_view());
        }

        i->AddArg(net_arg_ip_proto_, Variadic::String(ip_proto));

        base::StackString<16> tag("0x%x", evt.tag());
        i->AddArg(net_arg_tag_,
                  Variadic::String(
                      context_->storage->InternString(tag.string_view())));

        base::StackString<12> flags = GetTcpFlagMask(evt.tcp_flags());
        i->AddArg(net_arg_tcp_flags_,
                  Variadic::String(
                      context_->storage->InternString(flags.string_view())));

        i->AddArg(net_arg_local_port_, Variadic::Integer(evt.local_port()));
        i->AddArg(net_arg_remote_port_, Variadic::Integer(evt.remote_port()));
      });
}

void NetworkTraceModule::PushPacketBufferForSort(int64_t timestamp,
                                                 PacketSequenceState* state) {
  std::vector<uint8_t> v = packet_buffer_.SerializeAsArray();
  context_->sorter->PushTracePacket(
      timestamp, state->current_generation(),
      TraceBlobView(TraceBlob::CopyFrom(v.data(), v.size())));
  packet_buffer_.Reset();
}

}  // namespace trace_processor
}  // namespace perfetto
