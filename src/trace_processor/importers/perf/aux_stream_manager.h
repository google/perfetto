/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_STREAM_MANAGER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_STREAM_MANAGER_H_

#include <cstdint>
#include <map>
#include <memory>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_record.h"
#include "src/trace_processor/importers/perf/auxtrace_record.h"

namespace perfetto {
namespace trace_processor {
class TraceProcessorContext;

namespace perf_importer {

class AuxDataTokenizer;
class AuxDataTokenizerFactory;
struct Record;
class SampleId;
struct AuxtraceInfoRecord;

// Takes care of reconstructing the original data stream out of AUX and AUXTRACE
// records. Does not parse tha actual data it just forwards it to the associated
// `AuxDataTokenizer` .
class AuxStream {
 public:
  AuxStream(TraceProcessorContext* context,
            std::unique_ptr<AuxDataTokenizer> tokenizer);
  ~AuxStream();
  base::Status OnAuxRecord(AuxRecord aux);
  base::Status OnAuxtraceRecord(AuxtraceRecord auxtrace, TraceBlobView data);
  base::Status NotifyEndOfStream();

 private:
  class AuxtraceDataChunk {
   public:
    AuxtraceDataChunk(AuxtraceRecord auxtrace, TraceBlobView data)
        : auxtrace_(std::move(auxtrace)), data_(std::move(data)) {}

    TraceBlobView ConsumeFront(uint64_t size);
    void DropUntil(uint64_t offset);

    uint64_t offset() const { return auxtrace_.offset; }
    uint64_t end() const { return auxtrace_.offset + data_.size(); }
    uint64_t size() const { return data_.size(); }

   private:
    AuxtraceRecord auxtrace_;
    TraceBlobView data_;
  };

  base::Status MaybeParse();

  TraceProcessorContext* const context_;
  std::unique_ptr<AuxDataTokenizer> tokenizer_;
  base::CircularQueue<AuxRecord> outstanding_aux_records_;
  uint64_t aux_end_ = 0;
  base::CircularQueue<AuxtraceDataChunk> outstanding_auxtrace_data_;
  uint64_t auxtrace_end_ = 0;
  uint64_t tokenizer_offset_ = 0;
};

// Keeps track of all aux streams in a perf file.
class AuxStreamManager {
 public:
  explicit AuxStreamManager(TraceProcessorContext* context);
  ~AuxStreamManager();
  base::Status OnAuxtraceInfoRecord(AuxtraceInfoRecord info);
  base::Status OnAuxRecord(AuxRecord aux);
  base::Status OnAuxtraceRecord(AuxtraceRecord auxtrace, TraceBlobView data);

  base::Status FinalizeStreams();

 private:
  base::StatusOr<AuxStream*> GetOrCreateStreamForCpu(uint32_t cpu);

  TraceProcessorContext* context_;
  std::unique_ptr<AuxDataTokenizerFactory> tokenizer_factory_;
  std::map<uint32_t, AuxStream> auxdata_streams_by_cpu_;
};

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_STREAM_MANAGER_H_
