#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_EVENT_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_EVENT_H_

#include <cstdint>
#include <optional>

#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::primes {

// Temporary struct for the primes trace event so we can compile.
// TODO(leemh): Use the proto instead. Currently compiler can't find definitions...
struct alignas(8) PrimesTraceEvent {
  uint32_t tid;
  std::optional<StringPool::Id> comm;
  StringPool::Id method;
  enum { kEnter, kExit } action;
  std::optional<StringPool::Id> pathname;
  std::optional<uint32_t> line_number;
};

}  // namespace perfetto::trace_processor::primes

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PRIMES_PRIMES_TRACE_EVENT_H_
