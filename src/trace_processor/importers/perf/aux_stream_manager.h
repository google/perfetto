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
#include <functional>
#include <memory>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_data_tokenizer.h"
#include "src/trace_processor/importers/perf/aux_record.h"
#include "src/trace_processor/importers/perf/auxtrace_record.h"
#include "src/trace_processor/importers/perf/itrace_start_record.h"
#include "src/trace_processor/importers/perf/perf_session.h"
#include "src/trace_processor/importers/perf/time_conv_record.h"
#include "src/trace_processor/storage/stats.h"

namespace perfetto {
namespace trace_processor {
class TraceProcessorContext;

namespace perf_importer {

struct Record;
class SampleId;
struct AuxtraceInfoRecord;

class AuxStreamManager;

// Takes care of reconstructing the original data stream out of AUX and AUXTRACE
// records. Does not parse tha actual data it just forwards it to the associated
// `AuxDataTokenizer` .
class AuxStream {
 public:
  ~AuxStream();
  base::Status OnAuxRecord(AuxRecord aux);
  base::Status OnAuxtraceRecord(AuxtraceRecord auxtrace, TraceBlobView data);
  base::Status NotifyEndOfStream();
  base::Status OnItraceStartRecord(ItraceStartRecord start) {
    return tokenizer_->OnItraceStartRecord(std::move(start));
  }
  std::optional<uint64_t> ConvertTscToPerfTime(uint64_t cycles);

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

  friend AuxStreamManager;
  explicit AuxStream(AuxStreamManager* manager);

  base::Status MaybeParse();

  AuxStreamManager& manager_;
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
  explicit AuxStreamManager(TraceProcessorContext* context)
      : context_(context) {}
  base::Status OnAuxtraceInfoRecord(AuxtraceInfoRecord info);
  base::Status OnAuxRecord(AuxRecord aux);
  base::Status OnAuxtraceRecord(AuxtraceRecord auxtrace, TraceBlobView data);
  base::Status OnItraceStartRecord(ItraceStartRecord start);
  base::Status OnTimeConvRecord(TimeConvRecord time_conv) {
    time_conv_ = std::move(time_conv);
    return base::OkStatus();
  }

  base::Status FinalizeStreams();

  TraceProcessorContext* context() const { return context_; }

  std::optional<uint64_t> ConvertTscToPerfTime(uint64_t cycles) {
    if (!time_conv_) {
      context_->storage->IncrementStats(stats::perf_no_tsc_data);
      return std::nullopt;
    }
    return time_conv_->ConvertTscToPerfTime(cycles);
  }

 private:
  base::StatusOr<std::reference_wrapper<AuxStream>>
  GetOrCreateStreamForSampleId(const std::optional<SampleId>& sample_id);
  base::StatusOr<std::reference_wrapper<AuxStream>> GetOrCreateStreamForCpu(
      uint32_t cpu);

  TraceProcessorContext* const context_;
  std::unique_ptr<AuxDataTokenizerFactory> tokenizer_factory_;
  base::FlatHashMap<uint32_t, std::unique_ptr<AuxStream>>
      auxdata_streams_by_cpu_;
  std::optional<TimeConvRecord> time_conv_;
};

inline std::optional<uint64_t> AuxStream::ConvertTscToPerfTime(
    uint64_t cycles) {
  return manager_.ConvertTscToPerfTime(cycles);
}

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_AUX_STREAM_MANAGER_H_
