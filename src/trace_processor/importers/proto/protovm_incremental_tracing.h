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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_INCREMENTAL_TRACING_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_INCREMENTAL_TRACING_H_

#include <map>
#include <memory>
#include <vector>

#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/protovm/vm.h"

namespace perfetto {

namespace protovm {
class Vm;
}

namespace trace_processor {

class ProtoVmIncrementalTracing {
 public:
  void ProcessTraceProvenancePacket(protozero::ConstBytes blob);
  void InstantiateProtoVms(protozero::ConstBytes blob);
  protovm::StatusOr<TraceBlob> TryProcessPatch(const TraceBlobView& packet);

 private:
  std::map<uint32_t, int32_t> sequence_id_to_producer_id_;
  std::multimap<int32_t, protovm::Vm*> producer_id_to_vm_;
  std::vector<std::unique_ptr<protovm::Vm>> vms_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTOVM_INCREMENTAL_TRACING_H_
