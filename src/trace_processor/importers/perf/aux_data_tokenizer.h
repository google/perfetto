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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_DATA_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_DATA_TOKENIZER_H_

#include <cstdint>
#include <memory>
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;
namespace perf_importer {

static constexpr uint32_t AUX_TYPE_ETM = 3;

struct AuxRecord;

class AuxDataTokenizer {
 public:
  virtual ~AuxDataTokenizer();
  virtual void OnDataLoss(uint64_t) = 0;
  virtual base::Status Parse(AuxRecord record, TraceBlobView data) = 0;
  virtual base::Status NotifyEndOfStream() = 0;
};

class AuxDataTokenizerFactory {
 public:
  virtual ~AuxDataTokenizerFactory();
  virtual base::StatusOr<std::unique_ptr<AuxDataTokenizer>> CreateForCpu(
      uint32_t cpu) = 0;
};

// Dummy factory that creates tokenizers that just discard data.
// Used to skip streams that we do not know how to parse.
class DummyAuxDataTokenizerFactory : public AuxDataTokenizerFactory {
 public:
  explicit DummyAuxDataTokenizerFactory(TraceProcessorContext* context)
      : context_(context) {}
  base::StatusOr<std::unique_ptr<AuxDataTokenizer>> CreateForCpu(
      uint32_t cpu) override;

 private:
  TraceProcessorContext* const context_;
};

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_DATA_TOKENIZER_H_
