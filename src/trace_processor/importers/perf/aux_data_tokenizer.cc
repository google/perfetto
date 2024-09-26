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
#include "src/trace_processor/importers/perf/aux_stream_manager.h"
#include "src/trace_processor/importers/perf/itrace_start_record.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::perf_importer {

AuxDataTokenizer::~AuxDataTokenizer() = default;
AuxDataTokenizerFactory::~AuxDataTokenizerFactory() = default;

DummyAuxDataTokenizer::DummyAuxDataTokenizer(TraceProcessorContext* context,
                                             AuxStream*)
    : context_(context) {}

void DummyAuxDataTokenizer::OnDataLoss(uint64_t size) {
  context_->storage->IncrementStats(stats::perf_aux_lost,
                                    static_cast<int>(size));
}
base::Status DummyAuxDataTokenizer::Parse(AuxRecord, TraceBlobView data) {
  context_->storage->IncrementStats(stats::perf_aux_ignored,
                                    static_cast<int>(data.size()));
  return base::OkStatus();
}
base::Status DummyAuxDataTokenizer::NotifyEndOfStream() {
  return base::OkStatus();
}

base::Status DummyAuxDataTokenizer::OnItraceStartRecord(ItraceStartRecord) {
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::perf_importer
