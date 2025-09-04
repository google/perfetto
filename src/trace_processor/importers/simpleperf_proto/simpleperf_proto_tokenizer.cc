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

#include "src/trace_processor/importers/simpleperf_proto/simpleperf_proto_tokenizer.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::simpleperf_proto_importer {

namespace {
constexpr char kSimpleperfMagic[] = "SIMPLEPERF";
constexpr size_t kSimpleperfMagicSize = 10;
constexpr size_t kVersionSize = 2;
constexpr size_t kRecordSizeSize = 4;
}  // namespace

SimpleperfProtoTokenizer::SimpleperfProtoTokenizer(TraceProcessorContext* context)
    : context_(context) {}

SimpleperfProtoTokenizer::~SimpleperfProtoTokenizer() = default;

base::Status SimpleperfProtoTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));

  for (;;) {
    switch (state_) {
      case State::kExpectingMagic:
        RETURN_IF_ERROR(ParseMagic());
        break;

      case State::kExpectingVersion:
        RETURN_IF_ERROR(ParseVersion());
        break;

      case State::kExpectingRecordSize:
        RETURN_IF_ERROR(ParseRecordSize());
        break;

      case State::kExpectingRecord:
        RETURN_IF_ERROR(ParseRecord());
        break;

      case State::kFinished:
        return base::OkStatus();
    }
  }
}

base::Status SimpleperfProtoTokenizer::NotifyEndOfFile() {
  if (state_ != State::kFinished) {
    return base::ErrStatus("Unexpected end of simpleperf_proto file");
  }
  return base::OkStatus();
}

base::Status SimpleperfProtoTokenizer::ParseMagic() {
  auto iter = reader_.GetIterator();
  auto magic_data = iter.MaybeRead(kSimpleperfMagicSize);
  if (!magic_data) {
    return base::ErrStatus("Need more data");
  }

  if (std::memcmp(magic_data->data(), kSimpleperfMagic, kSimpleperfMagicSize) != 0) {
    return base::ErrStatus("Invalid simpleperf magic header");
  }

  reader_.PopFrontUntil(iter.file_offset());
  state_ = State::kExpectingVersion;
  return base::OkStatus();
}

base::Status SimpleperfProtoTokenizer::ParseVersion() {
  auto iter = reader_.GetIterator();
  auto version_data = iter.MaybeRead(kVersionSize);
  if (!version_data) {
    return base::ErrStatus("Need more data");
  }

  uint16_t version = *reinterpret_cast<const uint16_t*>(version_data->data());
  if (version != 1) {
    return base::ErrStatus("Unsupported simpleperf version: %d", version);
  }

  reader_.PopFrontUntil(iter.file_offset());
  state_ = State::kExpectingRecordSize;
  return base::OkStatus();
}

base::Status SimpleperfProtoTokenizer::ParseRecordSize() {
  auto iter = reader_.GetIterator();
  auto size_data = iter.MaybeRead(kRecordSizeSize);
  if (!size_data) {
    return base::ErrStatus("Need more data");
  }

  current_record_size_ = *reinterpret_cast<const uint32_t*>(size_data->data());

  reader_.PopFrontUntil(iter.file_offset());
  if (current_record_size_ == 0) {
    // End of records marker
    state_ = State::kFinished;
    return base::OkStatus();
  }

  state_ = State::kExpectingRecord;
  return base::OkStatus();
}

base::Status SimpleperfProtoTokenizer::ParseRecord() {
  auto iter = reader_.GetIterator();
  auto record_data = iter.MaybeRead(current_record_size_);
  if (!record_data) {
    return base::ErrStatus("Need more data");
  }

  // TODO: Parse the protobuf record here
  // For now, just skip the record data
  
  reader_.PopFrontUntil(iter.file_offset());
  state_ = State::kExpectingRecordSize;
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::simpleperf_proto_importer