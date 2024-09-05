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

#include "src/trace_processor/importers/perf/aux_stream_manager.h"
#include <cstdint>
#include <memory>
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_data_tokenizer.h"
#include "src/trace_processor/importers/perf/auxtrace_info_record.h"
#include "src/trace_processor/importers/perf/auxtrace_record.h"
#include "src/trace_processor/importers/perf/etm_tokenizer.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::perf_importer {

AuxStreamManager::AuxStreamManager(TraceProcessorContext* context)
    : context_(context) {}
AuxStreamManager::~AuxStreamManager() = default;

base::Status AuxStreamManager::OnAuxtraceInfoRecord(AuxtraceInfoRecord info) {
  if (tokenizer_factory_) {
    return base::ErrStatus("Multiple PERF_RECORD_AUXTRACE_INFO not supported.");
  }

  switch (info.type) {
    case AUX_TYPE_ETM: {
      ASSIGN_OR_RETURN(
          tokenizer_factory_,
          CreateEtmTokenizerFactory(context_, std::move(info.payload)));
      break;
    }
    default:
      context_->storage->IncrementIndexedStats(stats::perf_unknown_aux_data,
                                               static_cast<int>(info.type));

      tokenizer_factory_ =
          std::make_unique<DummyAuxDataTokenizerFactory>(context_);
      break;
  }
  return base::OkStatus();
}

base::Status AuxStreamManager::OnAuxRecord(AuxRecord aux) {
  if (!tokenizer_factory_) {
    return base::ErrStatus(
        "PERF_RECORD_AUX without previous PERF_RECORD_AUXTRACE_INFO.");
  }

  if (!aux.sample_id.has_value() || !aux.sample_id->cpu().has_value()) {
    return base::ErrStatus(
        "Aux data handling only implemented for per cpu data.");
  }
  ASSIGN_OR_RETURN(AuxStream * stream,
                   GetOrCreateStreamForCpu(*aux.sample_id->cpu()));
  return stream->OnAuxRecord(aux);
}

base::Status AuxStreamManager::OnAuxtraceRecord(AuxtraceRecord auxtrace,
                                                TraceBlobView data) {
  if (!tokenizer_factory_) {
    return base::ErrStatus(
        "PERF_RECORD_AUXTRACE without previous PERF_RECORD_AUXTRACE_INFO.");
  }
  if (auxtrace.cpu == std::numeric_limits<uint32_t>::max()) {
    // Aux data can be written by cpu or by tid. An unset cpu will have a value
    // of UINT32_MAX. Be aware for an unset tid simpleperf uses 0 and perf uses
    // UINT32_MAX. ¯\_(ツ)_/¯
    // Deal just with per cpu data for now.
    return base::ErrStatus(
        "Aux data handling only implemented for per cpu data.");
  }
  ASSIGN_OR_RETURN(AuxStream * stream, GetOrCreateStreamForCpu(auxtrace.cpu));
  return stream->OnAuxtraceRecord(std::move(auxtrace), std::move(data));
}

base::Status AuxStreamManager::FinalizeStreams() {
  for (auto it = auxdata_streams_by_cpu_.begin();
       it != auxdata_streams_by_cpu_.end(); ++it) {
    RETURN_IF_ERROR(it->second.NotifyEndOfStream());
  }

  return base::OkStatus();
}

base::StatusOr<AuxStream*> AuxStreamManager::GetOrCreateStreamForCpu(
    uint32_t cpu) {
  PERFETTO_CHECK(tokenizer_factory_);
  auto it = auxdata_streams_by_cpu_.find(cpu);
  if (it == auxdata_streams_by_cpu_.end()) {
    ASSIGN_OR_RETURN(std::unique_ptr<AuxDataTokenizer> tokenizer,
                     tokenizer_factory_->CreateForCpu(cpu));

    it = auxdata_streams_by_cpu_
             .emplace(std::piecewise_construct, std::make_tuple(cpu),
                      std::make_tuple(context_, std::move(tokenizer)))
             .first;
  }

  return &it->second;
}

AuxStream::AuxStream(TraceProcessorContext* context,
                     std::unique_ptr<AuxDataTokenizer> tokenizer)
    : context_(context), tokenizer_(std::move(tokenizer)) {}
AuxStream::~AuxStream() = default;

base::Status AuxStream::OnAuxRecord(AuxRecord aux) {
  if (aux.offset < aux_end_) {
    return base::ErrStatus("Overlapping AuxRecord");
  }
  if (aux.offset > aux_end_) {
    context_->storage->IncrementStats(
        stats::perf_aux_missing, static_cast<int64_t>(aux.offset - aux_end_));
  }
  outstanding_aux_records_.emplace_back(std::move(aux));
  aux_end_ = aux.end();
  return MaybeParse();
}

base::Status AuxStream::OnAuxtraceRecord(AuxtraceRecord auxtrace,
                                         TraceBlobView data) {
  PERFETTO_CHECK(auxtrace.size == data.size());
  if (auxtrace.offset < auxtrace_end_) {
    return base::ErrStatus("Overlapping AuxtraceData");
  }
  if (auxtrace.offset > auxtrace_end_) {
    context_->storage->IncrementStats(
        stats::perf_auxtrace_missing,
        static_cast<int64_t>(auxtrace.offset - auxtrace_end_));
  }
  outstanding_auxtrace_data_.emplace_back(std::move(auxtrace), std::move(data));
  auxtrace_end_ = outstanding_auxtrace_data_.back().end();
  return MaybeParse();
}

base::Status AuxStream::MaybeParse() {
  while (!outstanding_aux_records_.empty() &&
         !outstanding_auxtrace_data_.empty()) {
    const AuxRecord& aux_record = outstanding_aux_records_.front();
    AuxtraceDataChunk& auxtrace_data = outstanding_auxtrace_data_.front();

    // We need both auxtrace and aux, so we start at the biggest offset.
    const uint64_t start_offset =
        std::max(aux_record.offset, auxtrace_data.offset());

    if (tokenizer_offset_ < start_offset) {
      tokenizer_->OnDataLoss(start_offset - tokenizer_offset_);
      tokenizer_offset_ = start_offset;
    }

    // Not enough aux data at front of queue.
    if (start_offset >= aux_record.end()) {
      outstanding_aux_records_.pop_front();
      continue;
    }

    // Not enough auxtrace data at front of queue.
    if (start_offset >= auxtrace_data.end()) {
      outstanding_auxtrace_data_.pop_front();
      continue;
    }

    const uint64_t end_offset = std::min(aux_record.end(), auxtrace_data.end());
    const uint64_t size = end_offset - start_offset;

    PERFETTO_CHECK(tokenizer_offset_ == start_offset);
    PERFETTO_CHECK(start_offset != end_offset);

    auxtrace_data.DropUntil(start_offset);
    TraceBlobView data = auxtrace_data.ConsumeFront(size);

    AuxRecord adjusted_aux_record = aux_record;
    adjusted_aux_record.offset = tokenizer_offset_;
    adjusted_aux_record.size = size;
    tokenizer_offset_ += size;
    RETURN_IF_ERROR(
        tokenizer_->Parse(std::move(adjusted_aux_record), std::move(data)));
  }
  return base::OkStatus();
}

base::Status AuxStream::NotifyEndOfStream() {
  if (aux_end_ < auxtrace_end_) {
    context_->storage->IncrementStats(
        stats::perf_aux_missing,
        static_cast<int64_t>(auxtrace_end_ - aux_end_));
  } else if (auxtrace_end_ < aux_end_) {
    context_->storage->IncrementStats(
        stats::perf_auxtrace_missing,
        static_cast<int64_t>(aux_end_ - auxtrace_end_));
  }

  uint64_t end = std::max(aux_end_, auxtrace_end_);
  if (tokenizer_offset_ < end) {
    uint64_t loss = end - tokenizer_offset_;
    tokenizer_->OnDataLoss(loss);
    tokenizer_offset_ += loss;
  }
  return tokenizer_->NotifyEndOfStream();
}

void AuxStream::AuxtraceDataChunk::DropUntil(uint64_t offset) {
  PERFETTO_CHECK(offset >= this->offset() && offset <= end());
  const uint64_t size = offset - this->offset();

  data_ = data_.slice_off(size, data_.size() - size);
  auxtrace_.size -= size;
  auxtrace_.offset += size;
}

TraceBlobView AuxStream::AuxtraceDataChunk::ConsumeFront(uint64_t size) {
  PERFETTO_CHECK(size <= data_.size());
  TraceBlobView res = data_.slice_off(0, size);
  data_ = data_.slice_off(size, data_.size() - size);
  auxtrace_.size -= size;
  auxtrace_.offset += size;
  return res;
}

}  // namespace perfetto::trace_processor::perf_importer
