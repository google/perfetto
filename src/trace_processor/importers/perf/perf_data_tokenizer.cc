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

#include "src/trace_processor/importers/perf/perf_data_tokenizer.h"
#include <cstdint>
#include <cstring>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/perf/perf_data_reader.h"
#include "src/trace_processor/importers/perf/perf_data_tracker.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

namespace {
std::vector<uint64_t> ReadVectorFromBuffer(std::vector<uint8_t>& buffer,
                                           uint64_t buffer_offset,
                                           uint64_t size) {
  uint64_t bytes_from_buffer = buffer.size() - buffer_offset;
  PERFETTO_CHECK(sizeof(uint64_t) * size ==
                 sizeof(uint8_t) * bytes_from_buffer);

  std::vector<uint64_t> res(static_cast<size_t>(size));
  memcpy(res.data(), buffer.data() + buffer_offset,
         static_cast<size_t>(bytes_from_buffer));
  return res;
}

}  // namespace

PerfDataTokenizer::PerfDataTokenizer(TraceProcessorContext* ctx)
    : context_(ctx),
      tracker_(PerfDataTracker::GetOrCreate(context_)),
      reader_() {}

PerfDataTokenizer::~PerfDataTokenizer() = default;

base::Status PerfDataTokenizer::Parse(TraceBlobView blob) {
  reader_.Append(std::move(blob));

  while (parsing_state_ != ParsingState::Records) {
    base::StatusOr<ParsingResult> parsed = ParsingResult::Success;
    switch (parsing_state_) {
      case ParsingState::Records:
        break;
      case ParsingState::Header:
        parsed = ParseHeader();
        break;
      case ParsingState::AfterHeaderBuffer:
        parsed = ParseAfterHeaderBuffer();
        break;
      case ParsingState::Attrs:
        parsed = ParseAttrs();
        break;
      case ParsingState::AttrIdsFromBuffer:
        parsed = ParseAttrIdsFromBuffer();
        break;
      case ParsingState::AttrIds:
        parsed = ParseAttrIds();
        break;
    }

    // There has been an error while parsing.
    RETURN_IF_ERROR(parsed.status());

    // There is not enough data to parse so we need to load another blob.
    if (*parsed == ParsingResult::NoSpace)
      return base::OkStatus();
  }

  while (reader_.current_file_offset() < header_.data.end()) {
    // Make sure |perf_event_header| of the sample is available.
    if (!reader_.CanReadSize(sizeof(perf_event_header))) {
      return base::OkStatus();
    }

    perf_event_header ev_header;
    reader_.Peek(ev_header);
    PERFETTO_CHECK(ev_header.size >= sizeof(perf_event_header));

    if (!reader_.CanReadSize(ev_header.size)) {
      return base::OkStatus();
    }

    reader_.Skip<perf_event_header>();
    uint64_t record_offset = reader_.current_file_offset();
    uint64_t record_size = ev_header.size - sizeof(perf_event_header);

    switch (ev_header.type) {
      case PERF_RECORD_SAMPLE: {
        TraceBlobView tbv = reader_.PeekTraceBlobView(record_size);
        auto sample_status = tracker_->ParseSample(reader_);
        if (!sample_status.ok()) {
          continue;
        }
        PerfDataTracker::PerfSample sample = *sample_status;
        if (!ValidateSample(*sample_status)) {
          continue;
        }
        context_->sorter->PushTraceBlobView(
            static_cast<int64_t>(*sample_status->ts), std::move(tbv));
        break;
      }
      case PERF_RECORD_MMAP2: {
        PERFETTO_CHECK(ev_header.size >=
                       sizeof(PerfDataTracker::Mmap2Record::Numeric));
        auto record = ParseMmap2Record(record_size);
        RETURN_IF_ERROR(record.status());
        tracker_->PushMmap2Record(*record);
        break;
      }
      default:
        break;
    }

    reader_.Skip((record_offset + record_size) - reader_.current_file_offset());
  }

  return base::OkStatus();
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseHeader() {
  if (!reader_.CanReadSize(sizeof(PerfHeader))) {
    return ParsingResult::NoSpace;
  }
  reader_.Read(header_);
  PERFETTO_CHECK(header_.size == sizeof(PerfHeader));
  PERFETTO_CHECK(header_.attr_size ==
                 sizeof(perf_event_attr) +
                     sizeof(PerfDataTracker::PerfFileSection));

  if (header_.attrs.offset > header_.data.offset) {
    return base::ErrStatus(
        "Can only import files where samples are located after the "
        "metadata.");
  }

  if (header_.size == header_.attrs.offset) {
    parsing_state_ = ParsingState::Attrs;
  } else {
    parsing_state_ = ParsingState::AfterHeaderBuffer;
  }
  return ParsingResult::Success;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseAfterHeaderBuffer() {
  if (!reader_.CanAccessFileRange(header_.size, header_.attrs.offset)) {
    return ParsingResult::NoSpace;
  }
  after_header_buffer_.resize(
      static_cast<size_t>(header_.attrs.offset - header_.size) /
      sizeof(uint8_t));
  reader_.ReadVector(after_header_buffer_);
  parsing_state_ = ParsingState::Attrs;
  return ParsingResult::Success;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseAttrs() {
  if (!reader_.CanAccessFileRange(header_.attrs.offset, header_.attrs.end())) {
    return ParsingResult::NoSpace;
  }
  reader_.Skip(header_.attrs.offset - reader_.current_file_offset());
  PerfDataTracker::PerfFileAttr attr;
  for (uint64_t i = header_.attrs.offset; i < header_.attrs.end();
       i += header_.attr_size) {
    reader_.Read(attr);
    PERFETTO_CHECK(attr.ids.size % sizeof(uint64_t) == 0);
    ids_start_ = std::min(ids_start_, attr.ids.offset);
    ids_end_ = std::max(ids_end_, attr.ids.end());
    attrs_.push_back(attr);
  }

  if (ids_start_ == header_.size && ids_end_ <= header_.attrs.offset) {
    parsing_state_ = ParsingState::AttrIdsFromBuffer;
  } else {
    parsing_state_ = ParsingState::AttrIds;
  }
  return ParsingResult::Success;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseAttrIds() {
  if (!reader_.CanAccessFileRange(ids_start_, ids_end_)) {
    return ParsingResult::NoSpace;
  }
  for (const auto& attr_file : attrs_) {
    reader_.Skip(attr_file.ids.offset - reader_.current_file_offset());
    std::vector<uint64_t> ids(static_cast<size_t>(attr_file.ids.size) /
                              sizeof(uint64_t));
    reader_.ReadVector(ids);
    tracker_->PushAttrAndIds({attr_file.attr, std::move(ids)});
  }
  tracker_->ComputeCommonSampleType();

  // After parsing the ids we will parse the data.
  reader_.Skip(header_.data.offset - reader_.current_file_offset());
  parsing_state_ = ParsingState::Records;
  return ParsingResult::Success;
}

base::StatusOr<PerfDataTokenizer::ParsingResult>
PerfDataTokenizer::ParseAttrIdsFromBuffer() {
  for (const auto& attr_file : attrs_) {
    uint64_t size = attr_file.ids.size / sizeof(uint64_t);
    std::vector<uint64_t> ids = ReadVectorFromBuffer(
        after_header_buffer_, attr_file.ids.offset - ids_start_, size);
    tracker_->PushAttrAndIds({attr_file.attr, std::move(ids)});
  }
  after_header_buffer_.clear();
  tracker_->ComputeCommonSampleType();

  // After parsing the ids we will parse the data.
  reader_.Skip(header_.data.offset - reader_.current_file_offset());
  parsing_state_ = ParsingState::Records;
  return ParsingResult::Success;
}

base::StatusOr<PerfDataTracker::Mmap2Record>
PerfDataTokenizer::ParseMmap2Record(uint64_t record_size) {
  uint64_t start_offset = reader_.current_file_offset();
  PerfDataTracker::Mmap2Record record;
  reader_.Read(record.num);
  std::vector<char> filename_buffer(
      static_cast<size_t>(record_size) -
      sizeof(PerfDataTracker::Mmap2Record::Numeric));
  reader_.ReadVector(filename_buffer);
  if (filename_buffer.back() != '\0') {
    return base::ErrStatus(
        "Invalid MMAP2 record: filename is not null terminated.");
  }
  record.filename = std::string(filename_buffer.begin(), filename_buffer.end());
  PERFETTO_CHECK(reader_.current_file_offset() == start_offset + record_size);
  return record;
}

bool PerfDataTokenizer::ValidateSample(
    const PerfDataTracker::PerfSample& sample) {
  if (!sample.cpu.has_value() || !sample.ts.has_value() ||
      sample.callchain.empty() || !sample.pid.has_value()) {
    context_->storage->IncrementStats(stats::perf_samples_skipped);
    return false;
  }
  return true;
}

void PerfDataTokenizer::NotifyEndOfFile() {}

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto
