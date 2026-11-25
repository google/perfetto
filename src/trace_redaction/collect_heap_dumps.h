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

#ifndef SRC_TRACE_REDACTION_COLLECT_HEAP_DUMPS_H_
#define SRC_TRACE_REDACTION_COLLECT_HEAP_DUMPS_H_

#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

// Records, for every heap dump in the trace, how many packets it has and
// whether it is complete (see Context::heap_dump_sequences).
//
// This primitive never reads Context::package_uid, so the result does not
// depend on whether the package list appears before or after the heap dumps.
class CollectHeapDumps final : public CollectPrimitive {
 public:
  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       Context* context) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_COLLECT_HEAP_DUMPS_H_
