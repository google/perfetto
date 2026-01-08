#include <cstdint>

#include "perfetto/base/time.h"
#include "src/trace_processor/importers/primes/primes_trace_parser.h"
#include "src/trace_processor/importers/primes/primes_trace_tokenizer.h"

namespace primespb = perfetto::third_party::primes::pbzero;

namespace perfetto::trace_processor::primes {

constexpr uint32_t kStartTimeFieldNumber = 4;
constexpr uint32_t kEdgesFieldNumber = 7;
constexpr uint32_t kEdgeStartOffsetFieldNumber = 2;
constexpr uint32_t kSecondsFieldNumber = 1;
constexpr uint32_t kNanosFieldNumber = 2;

PrimesTraceTokenizer::PrimesTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx),
      stream_(
          ctx->sorter->CreateStream(std::make_unique<PrimesTraceParser>(ctx))) {
}

PrimesTraceTokenizer::~PrimesTraceTokenizer() = default;

int64_t to_nanos(int64_t seconds, int32_t nanos) {
  return seconds * 1000000000LL + nanos;
}

// Uses ProtoDecoder to buffer and parse the Trace message.
// 1. Buffers incoming TraceBlobView chunks until the trace start time can be
// extracted.
// 2. Extracts TraceEdge messages as TraceBlobView slices, and calculates their
// absolute timestamps using the start time.
// 3. Pushes (timestamp, TraceBlobView) pairs to the TraceSorter stream for full
// parsing.
base::Status PrimesTraceTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));
  size_t available_bytes = reader_.avail();
  auto slice = reader_.SliceOff(reader_.start_offset(), available_bytes);
  if (!slice.has_value()) {
    return base::ErrStatus(
        "Slicing TraceBlobView for Primes trace proto unexpectedly failed.");
  }
  auto decoder = protozero::ProtoDecoder(slice->data(), slice->size());

  // Start time needs to be extracted before the timestamp of any edge can be
  // calculated, as edge timestamps are stored as an offset to the trace start
  // time.
  if (!start_time_) {
    protozero::Field ts_field = decoder.FindField(kStartTimeFieldNumber);
    if (!ts_field) {
      // If the timestamp could not be found, return and wait for more data.
      // If the start time is never found, `NotifyEndOfFile` will return an
      // error.
      return base::OkStatus();
    }
    auto ts_bytes = ts_field.as_bytes();
    auto ts_decoder = protozero::ProtoDecoder(ts_bytes.data, ts_bytes.size);
    protozero::Field seconds_field = ts_decoder.FindField(kSecondsFieldNumber);
    if (!seconds_field || !seconds_field.valid()) {
      return base::ErrStatus("Trace start time is missing seconds field.");
    }
    protozero::Field nanos_field = ts_decoder.FindField(kNanosFieldNumber);
    if (!nanos_field || !nanos_field.valid()) {
      return base::ErrStatus("Trace start time is missing nanos field.");
    }
    start_time_ = to_nanos(seconds_field.as_int64(), nanos_field.as_int32());
  }

  size_t field_start_offset = decoder.read_offset();
  protozero::Field field = decoder.ReadField();
  size_t field_end_offset = decoder.read_offset();
  while (field.valid()) {
    // Pop the bytes only after a successful read to avoid losing data.
    reader_.PopFrontBytes(field_end_offset - field_start_offset);
    // Process only the edges fields. Other fields are not relevant to Perfetto.
    // To send to the trace parser we need to create a TraceBlobView slice of
    // the edge, and extract its timestamp relative to start_time_.
    if (field.id() == kEdgesFieldNumber) {
      auto field_bytes = field.as_bytes();
      auto start_offset_field =
          protozero::ProtoDecoder(field_bytes.data, field_bytes.size)
              .FindField(kEdgeStartOffsetFieldNumber)
              .as_bytes();
      auto ts_decoder = protozero::ProtoDecoder(start_offset_field.data,
                                                start_offset_field.size);
      auto seconds_field = ts_decoder.FindField(kSecondsFieldNumber);
      auto nanos_field = ts_decoder.FindField(kNanosFieldNumber);
      if (!seconds_field.valid() && !nanos_field.valid()) {
        PERFETTO_ELOG("Cannot calculate a valid timestamp for trace edge.");
        continue;  // Skip this edge.
      }
      int64_t edge_timestamp = start_time_ + to_nanos(seconds_field.as_int64(),
                                                      nanos_field.as_int32());

      TraceBlobView edge_slice = slice->slice_off(
          field_start_offset, field_end_offset - field_start_offset);
      stream_->Push(edge_timestamp, std::move(edge_slice));
    }
    // Read the next field
    field_start_offset = field_end_offset;
    field = decoder.ReadField();
    field_end_offset = decoder.read_offset();
  }
  return base::OkStatus();
}

base::Status PrimesTraceTokenizer::NotifyEndOfFile() {
  if (!start_time_) {
    return base::ErrStatus(
        "Did not find a valid trace start time before EOF. Malformed Primes "
        "trace proto?");
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::primes
