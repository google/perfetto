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

#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/proto/packet_analyzer.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/proto_profiler.h"

namespace perfetto {
namespace trace_processor {

// Interface for a module that processes track event information.
class ProtoContentAnalyzer : public PacketAnalyzer {
 public:
  struct Sample {
    size_t size;
    size_t count;
  };
  using PathToSamplesMap =
      base::FlatHashMap<util::SizeProfileComputer::FieldPath,
                        Sample,
                        util::SizeProfileComputer::FieldPathHasher>;
  using SampleAnnotation = PacketAnalyzer::SampleAnnotation;

  struct SampleAnnotationHasher {
    using argument_type = SampleAnnotation;
    using result_type = size_t;

    result_type operator()(const argument_type& p) const {
      base::Hasher hash;
      for (auto v : p) {
        hash.Update(v.first.raw_id());
        hash.Update(v.second.raw_id());
      }
      return static_cast<size_t>(hash.digest());
    }
  };
  using AnnotatedSamplesMap = base::
      FlatHashMap<SampleAnnotation, PathToSamplesMap, SampleAnnotationHasher>;

  ProtoContentAnalyzer(TraceProcessorContext* context);
  ~ProtoContentAnalyzer() override;

  void ProcessPacket(const TraceBlobView& packet,
                     const SampleAnnotation& annotation) override;

  void NotifyEndOfFile() override;

 private:
  TraceProcessorContext* context_;
  DescriptorPool pool_;
  util::SizeProfileComputer computer_;
  AnnotatedSamplesMap aggregated_samples_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CONTENT_ANALYZER_H_
