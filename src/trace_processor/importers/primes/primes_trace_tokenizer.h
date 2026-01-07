#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_TOKENIZER_H_

#include "perfetto/protozero/proto_decoder.h"
#include "protos/third_party/primes/primes_tracing.gen.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/primes/primes_trace_event.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::primes {

/**
 * Buffers an incoming Primes trace and tokenizes it into TraceEdge messages for
 * parsing.
 */
class PrimesTraceTokenizer : public ChunkedTraceReader {
 public:
  explicit PrimesTraceTokenizer(TraceProcessorContext*);
  ~PrimesTraceTokenizer() override;
  base::Status Parse(TraceBlobView) override;
  base::Status NotifyEndOfFile() override;

 private:
  util::TraceBlobViewReader reader_;
  TraceProcessorContext* const PERFETTO_UNUSED context_;
  std::unique_ptr<TraceSorter::Stream<TraceBlobView>> stream_;
  int64_t start_time_ = 0;
};

}  // namespace perfetto::trace_processor::primes

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_TOKENIZER_H_
