/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_HEAP_DUMP_UTIL_H_
#define SRC_TRACE_REDACTION_HEAP_DUMP_UTIL_H_

#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_redaction/filtering.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/third_party/android/art/heap_graph.pbzero.h"

namespace perfetto::trace_redaction {

// TracePacket.heap_graph is declared as an extension in
// protos/third_party/android/art/heap_graph.proto.
inline constexpr uint32_t kHeapGraphFieldNumber = com::android::art::tracing::
    pbzero::ArtHeapGraphTracePacket::kHeapGraphFieldNumber;

// Allowlist item for PruneMissingUidPackets: keeps a packet whose only data
// field is a single DeobfuscationMapping for Context::package_name. Requires
// `PackageValidation` in the pipeline (`!Context::package_name.empty()`).
class AllowTargetDeobfuscationMapping final : public PacketFilter {
 public:
  bool Includes(const Context& context,
                const std::string& packet) const override;
};

// Counts describing what heap dump redaction kept and removed. Derived from
// Context after a run, because transforms are stateless.
struct HeapDumpRedactionSummary {
  // Heap graph packets of complete dumps written by the target.
  uint32_t heap_graph_packets_kept = 0;

  // All other heap graph packets (other apps, system-uid processes,
  // incomplete target dumps).
  uint32_t heap_graph_packets_dropped = 0;

  // Number of complete target dumps kept. Zero means the output has no heap
  // data (not an error).
  uint32_t complete_dumps_kept = 0;

  // Number of target dumps removed because they were incomplete.
  uint32_t incomplete_dumps_dropped = 0;

  // Packets written by app uids other than the target (any payload).
  uint32_t untrusted_app_packets_dropped = 0;

  // Packets without a trusted_uid that are not the target's mapping.
  uint32_t missing_uid_packets_dropped = 0;

  // Deobfuscation mappings kept for the target package.
  uint32_t deobfuscation_mappings_kept = 0;
};

HeapDumpRedactionSummary SummarizeHeapDumpRedaction(const Context& context);

// Heap dump specific rules shared by the heap dump primitives. Generic packet
// and uid helpers live in trace_redaction_framework.h.
namespace heap_dump {

// Returns the package name of a packet whose only data field is a single
// DeobfuscationMapping with a single package name, or nullopt otherwise.
std::optional<protozero::ConstChars> DeobfuscationMappingPackage(
    protozero::ProtoDecoder* packet);

}  // namespace heap_dump
}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_HEAP_DUMP_UTIL_H_
