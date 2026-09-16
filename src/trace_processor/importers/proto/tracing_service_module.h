/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACING_SERVICE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACING_SERVICE_MODULE_H_

#include <cstdint>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Handles the packets the tracing service emits about the trace itself and
// that nothing else in the pipeline depends on: service lifecycle events
// and extension descriptors. Clock snapshots stay in ProtoTraceReader,
// since resolving any packet's timestamp depends on them.
class TracingServiceModule : public ProtoImporterModule {
 public:
  using ConstBytes = protozero::ConstBytes;

  TracingServiceModule(ProtoImporterModuleContext* module_context,
                       TraceProcessorContext* context);
  ~TracingServiceModule() override;

  ModuleResult TokenizePacket(const TokenizePacketArgs& args) override;

 private:
  base::Status ParseServiceEvent(int64_t ts, ConstBytes blob);
  base::Status ParseExtensionDescriptor(ConstBytes descriptor);

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACING_SERVICE_MODULE_H_
