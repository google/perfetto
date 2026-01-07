#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_PARSER_H_

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "protos/third_party/primes/primes_tracing.pbzero.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/importers/primes/primes_trace_event.h"
#include "protos/third_party/primes/primes_tracing.gen.h"
#include "src/trace_processor/importers/primes/primes_trace_event.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor::primes {
namespace primespb = perfetto::third_party::primes::pbzero;

/// Holds all state needed while parsing through Primes trace edges.
struct ParsingState {
  std::unordered_map<uint64_t, StringId> edge_id_to_slice_name_map;
  std::unordered_map<uint64_t, SliceId> edge_id_to_slice_id_map;
  std::unordered_map<uint64_t, TrackId> edge_id_to_track_id_map;
};

class PrimesTraceParser
    : public TraceSorter::Sink<TraceBlobView, PrimesTraceParser> {
 public:
  explicit PrimesTraceParser(TraceProcessorContext*);
  ~PrimesTraceParser() override;

  void Parse(int64_t ts, TraceBlobView trace_edge);

 private:
  TraceProcessorContext* const context_;
  ParsingState parsing_state_;

  void HandleSliceBegin(int64_t ts, primespb::TraceEdge_Decoder& edge_decoder);
  void HandleSliceEnd(int64_t ts, primespb::TraceEdge_Decoder& edge_decoder);
  void HandleMark(int64_t ts, primespb::TraceEdge_Decoder& edge_decoder);
  void HandleFlows(SliceId slice_id,
                   const primespb::TraceEdge_TraceEntityDetails_Decoder& details);

  /// Given an edge's ID, attempts to resolve the thread it belongs to, if one
  /// exists for it.
  std::optional<UniqueTid> ResolveThreadId(uint64_t edge_id,
                                           uint64_t parent_id);
};

}  // namespace perfetto::trace_processor::primes

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_PARSER_H_