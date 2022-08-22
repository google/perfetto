/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CONTENT_ANALYZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CONTENT_ANALYZER_H_

#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/proto_profiler.h"

namespace perfetto {
namespace trace_processor {

// Computes a trace proto size breakdown by field path, and exports the data to
// an SQL table.
class ContentAnalyzerModule : public ProtoImporterModule {
 public:
  explicit ContentAnalyzerModule(TraceProcessorContext* context);

  ~ContentAnalyzerModule() override = default;

  ModuleResult TokenizePacket(const protos::pbzero::TracePacket_Decoder&,
                              TraceBlobView* packet,
                              int64_t packet_timestamp,
                              PacketSequenceState*,
                              uint32_t field_id) override;

  void NotifyEndOfFile() override;

 private:
  TraceProcessorContext* context_;
  DescriptorPool pool_;
  util::SizeProfileComputer::PathToSamplesMap aggregated_samples_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CONTENT_ANALYZER_H_
