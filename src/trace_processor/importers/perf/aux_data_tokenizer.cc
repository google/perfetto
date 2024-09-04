/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/perf/aux_data_tokenizer.h"

#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_record.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::perf_importer {
namespace {
class DummyAuxDataTokenizer : public AuxDataTokenizer {
 public:
  explicit DummyAuxDataTokenizer(TraceProcessorContext* context)
      : context_(context) {}
  void OnDataLoss(uint64_t size) override {
    context_->storage->IncrementStats(stats::perf_aux_lost,
                                      static_cast<int>(size));
  }
  base::Status Parse(AuxRecord, TraceBlobView data) override {
    context_->storage->IncrementStats(stats::perf_aux_ignored,
                                      static_cast<int>(data.size()));
    return base::OkStatus();
  }
  base::Status NotifyEndOfStream() override { return base::OkStatus(); }

 private:
  TraceProcessorContext* const context_;
};
}  // namespace

AuxDataTokenizer::~AuxDataTokenizer() = default;
AuxDataTokenizerFactory::~AuxDataTokenizerFactory() = default;

base::StatusOr<std::unique_ptr<AuxDataTokenizer>>
DummyAuxDataTokenizerFactory::CreateForCpu(uint32_t) {
  return std::unique_ptr<AuxDataTokenizer>(new DummyAuxDataTokenizer(context_));
}

}  // namespace perfetto::trace_processor::perf_importer
