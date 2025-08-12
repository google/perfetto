#ifndef SRC_TRACE_PROCESSOR_TYPES_TRACE_PROCESSOR_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TYPES_TRACE_PROCESSOR_CONTEXT_H_

#include <memory>
#include "src/trace_processor/types/per_global_context.h"
#include "src/trace_processor/types/per_machine_context.h"
#include "src/trace_processor/types/per_trace_context.h"

namespace perfetto::trace_processor {

struct TraceProcessorContext {
  std::unique_ptr<PerGlobalContext> global_context;
  std::unique_ptr<PerMachineContext> machine_context;
  std::unique_ptr<PerTraceContext> trace_context;

  // Constructor for production use, called from TraceProcessorStorageImpl.
  explicit TraceProcessorContext(const PerGlobalContext::InitArgs&);

  // Default constructor for testing purposes.
  TraceProcessorContext();
  ~TraceProcessorContext();

  TraceProcessorContext(TraceProcessorContext&&);
  TraceProcessorContext& operator=(TraceProcessorContext&&);
};

}  // namespace perfetto::trace_processor
#endif  // SRC_TRACE_PROCESSOR_TYPES_TRACE_PROCESSOR_CONTEXT_H_
