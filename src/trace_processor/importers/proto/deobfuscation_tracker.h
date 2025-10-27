/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_DEOBFUSCATION_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_DEOBFUSCATION_TRACKER_H_

#include <optional>
#include <vector>

#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

class DeobfuscationTracker : public Destructible {
 public:
  explicit DeobfuscationTracker(TraceProcessorContext* context);
  ~DeobfuscationTracker() override;

  static DeobfuscationTracker* Get(TraceProcessorContext* context) {
    return static_cast<DeobfuscationTracker*>(
        context->deobfuscation_tracker.get());
  }

  void AddDeobfuscationMapping(protozero::ConstBytes blob);
  void NotifyEndOfFile();

 private:
  void DeobfuscateProfiles(
      const protos::pbzero::DeobfuscationMapping::Decoder& mapping);
  void ParseDeobfuscationMappingForHeapGraph(
      const protos::pbzero::DeobfuscationMapping::Decoder& mapping);
  void DeobfuscateHeapGraphClass(
      std::optional<StringId> package_name_id,
      StringId obfuscated_class_name_id,
      const protos::pbzero::ObfuscatedClass::Decoder& cls);
  void GuessPackages();
  void GuessPackageForCallsite(
      tables::ProcessTable::Id upid,
      tables::StackProfileCallsiteTable::Id callsite_id);

  std::vector<TraceBlob> packets_;
  TraceProcessorContext* context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_DEOBFUSCATION_TRACKER_H_
