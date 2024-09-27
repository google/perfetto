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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_SPE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_SPE_TOKENIZER_H_

#include <cstdint>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_data_tokenizer.h"
#include "src/trace_processor/importers/perf/aux_record.h"
#include "src/trace_processor/importers/perf/aux_stream_manager.h"
#include "src/trace_processor/importers/perf/perf_session.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto ::trace_processor {
class TraceProcessorContext;
namespace perf_importer {

class SpeTokenizer : public AuxDataTokenizer {
 public:
  explicit SpeTokenizer(TraceProcessorContext* context, AuxStream* stream)
      : context_(context), stream_(*stream) {}
  void OnDataLoss(uint64_t) override;
  base::Status Parse(AuxRecord record, TraceBlobView data) override;
  base::Status NotifyEndOfStream() override;
  base::Status OnItraceStartRecord(ItraceStartRecord) override;

 private:
  // A SPE trace is just a stream of SPE records which in turn are a collection
  // of packets. An End or Timestamp packet signals the end of the current
  // record. This method will read the stream until an end of record condition,
  // emit the record to the sorter, consume the bytes from the buffer, and
  // finally return true. If not enough data is available to parse a full record
  // it returns false and the internal buffer is not modified.
  bool ProcessRecord();
  uint64_t ReadTimestamp(const TraceBlobView& record);

  // Emits a record to the sorter. You can optionally pass the cycles value
  // contained in the timestamp packet which will be used to determine the trace
  // timestamp.
  void Emit(TraceBlobView data, std::optional<uint64_t> cycles);
  TraceProcessorContext* const context_;
  AuxStream& stream_;
  util::TraceBlobViewReader buffer_;
  std::optional<AuxRecord> last_aux_record_;
};

using SpeTokenizerFactory = SimpleAuxDataTokenizerFactory<SpeTokenizer>;

}  // namespace perf_importer
}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_SPE_TOKENIZER_H_
