#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/importers/common/machine_tracker.h"

namespace perfetto::trace_processor {

// This constructor is called from TraceProcessorStorageImpl and is the main
// entry point for initializing the entire context hierarchy.
TraceProcessorContext::TraceProcessorContext(
    const PerGlobalContext::InitArgs& args) {
  // 1. Create the context objects.
  global_context = std::make_unique<PerGlobalContext>(args);
  machine_context = std::make_unique<PerMachineContext>();
  trace_context = std::make_unique<PerTraceContext>();

  // 2. Initialize the trackers within each context. The `Init` calls pass
  //    a pointer to this `TraceProcessorContext`, allowing trackers to access
  //    other contexts if needed (e.g., accessing global storage from a
  //    per-trace tracker).
  trace_context->Init(this);
  machine_context->Init(this, args.raw_machine_id);
  // Global needs to be initialzed after machine.
  bool create_multi_machine_manager =
      !machine_context->machine_tracker ||
      !machine_context->machine_tracker->machine_id();
  global_context->Init(this, create_multi_machine_manager);
}

// Default constructor for testing.
TraceProcessorContext::TraceProcessorContext() {
  trace_context = std::make_unique<PerTraceContext>();
  machine_context = std::make_unique<PerMachineContext>();
  global_context = std::make_unique<PerGlobalContext>();
}

TraceProcessorContext::~TraceProcessorContext() = default;
TraceProcessorContext::TraceProcessorContext(TraceProcessorContext&&) = default;
TraceProcessorContext& TraceProcessorContext::operator=(
    TraceProcessorContext&&) = default;

}  // namespace perfetto::trace_processor
