#include <cstdint>

#include "protos/third_party/primes/primes_tracing.pbzero.h"
#include "src/trace_processor/importers/primes/primes_trace_parser.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "perfetto/base/compiler.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace primespb = perfetto::third_party::primes::pbzero;
namespace tracks = perfetto::trace_processor::tracks;

namespace perfetto::trace_processor::primes {

static constexpr auto kBlueprint = tracks::SliceBlueprint(
    "primes_track",
    tracks::DimensionBlueprints(tracks::kThreadDimensionBlueprint),
    tracks::DynamicNameBlueprint());

PrimesTraceParser::PrimesTraceParser(TraceProcessorContext* ctx)
    : context_(ctx) {}

PrimesTraceParser::~PrimesTraceParser() = default;

void PrimesTraceParser::Parse(int64_t ts, TraceBlobView trace_edge) {
  auto proto_decoder =
      protozero::ProtoDecoder(trace_edge.data(), trace_edge.length());
  auto field_bytes = proto_decoder.ReadField().as_bytes();
  auto edge_decoder =
      primespb::TraceEdge_Decoder(field_bytes.data, field_bytes.size);

  if (edge_decoder.has_slice_begin()) {
    HandleSliceBegin(ts, edge_decoder);
  } else if (edge_decoder.has_slice_end()) {
    HandleSliceEnd(ts, edge_decoder);
  } else if (edge_decoder.has_mark()) {
    HandleMark(ts, edge_decoder);
  } else {
    PERFETTO_ELOG("Unknown trace edge type for Primes Trace Edge{id=%" PRIu64
                  "}",
                  edge_decoder.id());
  }
}

void PrimesTraceParser::HandleSliceBegin(
    int64_t ts,
    primespb::TraceEdge_Decoder& edge_decoder) {
  auto sb_decoder =
      primespb::TraceEdge_SliceBegin_Decoder(edge_decoder.slice_begin());
  auto details_decoder = primespb::TraceEdge_TraceEntityDetails_Decoder(
      sb_decoder.entity_details().data, sb_decoder.entity_details().size);

  // Primes executors are mapped to Perfetto tracks (with each track having its
  // own thread).
  TrackId track_id;

  // If this edge has its own executor ID that means it's the root slice for the
  // executor. Create a new thread and track for it. Otherwise, inherit the
  // executor ID from the parent slice.
  if (sb_decoder.has_executor_id()) {
    UniqueTid utid = context_->process_tracker->GetOrCreateThread(
        (int64_t)sb_decoder.executor_id());
    StringId executor_name =
        context_->storage->InternString(sb_decoder.executor_name());
    track_id = context_->track_tracker->InternTrack(
        kBlueprint, tracks::Dimensions(utid), executor_name);
    parsing_state_.edge_id_to_track_id_map[edge_decoder.id()] = track_id;
  } else {
    // The parent ought to have already been processed. If not, log an error and
    // return.
    auto it = parsing_state_.edge_id_to_track_id_map.find(
        details_decoder.parent_id());
    if (it == parsing_state_.edge_id_to_track_id_map.end()) {
      PERFETTO_ELOG("Could not find parent track id for edge %" PRIu64,
                    edge_decoder.id());
      return;
    }
    track_id = it->second;
    parsing_state_.edge_id_to_track_id_map[edge_decoder.id()] = track_id;
  }

  // Now that the track is known, create a new slice on that track.
  auto slice_name = context_->storage->InternString(details_decoder.name());
  parsing_state_.edge_id_to_slice_name_map[edge_decoder.id()] = slice_name;

  std::optional<SliceId> slice_id =
      context_->slice_tracker->Begin(ts, track_id, kNullStringId, slice_name);
  if (!slice_id) {
    PERFETTO_ELOG("Failed to begin slice for edge_id=%" PRIu64,
                  edge_decoder.id());
    return;
  }
  parsing_state_.edge_id_to_slice_id_map[edge_decoder.id()] = slice_id.value();

  HandleFlows(*slice_id, details_decoder);
}

void PrimesTraceParser::HandleSliceEnd(
    int64_t ts,
    primespb::TraceEdge_Decoder& edge_decoder) {
  // A SliceEnd edge has the same ID as the corresponding SliceBegin edge.
  // The parent ought to have already been processed. If not, log an error and
  // return.
  TrackId track_id;
  {
    auto it = parsing_state_.edge_id_to_track_id_map.find(edge_decoder.id());
    if (it == parsing_state_.edge_id_to_track_id_map.end()) {
      PERFETTO_ELOG("Could not find track id for end slice %" PRIu64,
                    edge_decoder.id());
      return;
    }
    track_id = it->second;
  }
  StringId slice_name;
  {
    auto it = parsing_state_.edge_id_to_slice_name_map.find(edge_decoder.id());
    if (it == parsing_state_.edge_id_to_slice_name_map.end()) {
      PERFETTO_ELOG("Could not find slice name for end slice %" PRIu64,
                    edge_decoder.id());
      return;
    }
    slice_name = it->second;
  }
  context_->slice_tracker->End(ts, track_id, kNullStringId, slice_name);
}

void PrimesTraceParser::HandleMark(int64_t ts,
                                   primespb::TraceEdge_Decoder& edge_decoder) {
  auto mark_decoder = primespb::TraceEdge_Mark_Decoder(edge_decoder.mark());
  if (!mark_decoder.has_entity_details()) {
    PERFETTO_ELOG("Invalid Mark{id=%" PRIu64 "} found (no entity_details)",
                  edge_decoder.id());
    return;
  }
  auto details_decoder = primespb::TraceEdge_TraceEntityDetails_Decoder(
      mark_decoder.entity_details());
  if (!details_decoder.has_parent_id()) {
    PERFETTO_ELOG("Invalid Mark{id=%" PRIu64 "} found (no parent_id)",
                  edge_decoder.id());
    return;
  }

  auto parent_id = details_decoder.parent_id();
  TrackId track_id = parsing_state_.edge_id_to_track_id_map[parent_id];
  auto slice_name = context_->storage->InternString(details_decoder.name());

  // A mark is a slice with zero duration.
  std::optional<SliceId> slice_id = context_->slice_tracker->Scoped(
      ts, track_id, kNullStringId, slice_name, 0);
  if (!slice_id) {
    PERFETTO_ELOG("Failed to add mark slice for edge_id=%" PRIu64,
                  edge_decoder.id());
    return;
  }
  parsing_state_.edge_id_to_slice_id_map[edge_decoder.id()] = slice_id.value();

  HandleFlows(*slice_id, details_decoder);
}

// Handles both "follows_from" relationships (which are direct, causal links
// between two specific slices, A -> B) and "flow_ids" (which are shared
// identifiers linking a chain of events across threads/processes,
// e.g., A -> B -> C).
//
// For follows_from: Creates a direct flow from the leader slice to the current
// slice. For flow_ids: Manages the flow chain state (Begin/Step) to link the
// current slice to the previous slice in the same flow chain.
void PrimesTraceParser::HandleFlows(
    SliceId slice_id,
    const primespb::TraceEdge_TraceEntityDetails_Decoder& details_decoder) {
  // Convert follows-from relationships into flows.
  if (details_decoder.has_follows_from_ids()) {
    for (auto it = details_decoder.follows_from_ids(); it; ++it) {
      uint64_t follows_from_id = it->as_uint64();
      auto leader_it =
          parsing_state_.edge_id_to_slice_id_map.find(follows_from_id);
      if (leader_it != parsing_state_.edge_id_to_slice_id_map.end()) {
        SliceId leader_slice_id = leader_it->second;
        // The "leader" slice is the outgoing flow, and the current
        // slice is the incoming one.
        context_->flow_tracker->InsertFlow(leader_slice_id, slice_id);
      }
    }
  }

  if (details_decoder.has_flow_ids()) {
    for (auto it = details_decoder.flow_ids(); it; ++it) {
      uint64_t flow_id = it->as_uint64();
      if (context_->flow_tracker->IsActive(flow_id)) {
        context_->flow_tracker->Step(slice_id, flow_id);
      } else {
        context_->flow_tracker->Begin(slice_id, flow_id);
      }
    }
  }
}

}  // namespace perfetto::trace_processor::primes
