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

#include "src/trace_processor/importers/archive/tar_trace_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/archive/archive_entry.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {
namespace {

constexpr char kUstarMagic[] = {'u', 's', 't', 'a', 'r', '\0'};
constexpr char kGnuMagic[] = {'u', 's', 't', 'a', 'r', ' ', ' ', '\0'};

constexpr char TYPE_FLAG_REGULAR = '0';
constexpr char TYPE_FLAG_AREGULAR = '\0';
constexpr char TYPE_FLAG_GNU_LONG_NAME = 'L';

template <size_t Size>
std::optional<uint64_t> ExtractUint64(const char (&ptr)[Size]) {
  static_assert(Size <= 64 / 3);
  if (*ptr == 0) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < Size && ptr[i] != 0; ++i) {
    if (ptr[i] > '7' || ptr[i] < '0') {
      return std::nullopt;
    }
    value <<= 3;
    value += static_cast<uint64_t>(ptr[i] - '0');
  }
  return value;
}

enum class TarType { kUnknown, kUstar, kGnu };

struct alignas(1) Header {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char checksum[8];
  char type_flag[1];
  char link_name[100];
  union {
    struct UstarMagic {
      char magic[6];
      char version[2];
    } ustar;
    char gnu[8];
  } magic;
  char user_name[32];
  char group_name[32];
  char dev_major[8];
  char dev_minor[8];
  char prefix[155];
  char padding[12];

  TarType GetTarFileType() const {
    if (memcmp(magic.gnu, kGnuMagic, sizeof(kGnuMagic)) == 0) {
      return TarType::kGnu;
    }
    if (memcmp(magic.ustar.magic, kUstarMagic, sizeof(kUstarMagic)) == 0) {
      return TarType::kUstar;
    }
    return TarType::kUnknown;
  }
};

constexpr size_t kHeaderSize = 512;
static_assert(sizeof(Header) == kHeaderSize);

bool IsAllZeros(const TraceBlobView& data) {
  const uint8_t* start = data.data();
  const uint8_t* end = data.data() + data.size();
  return std::find_if(start, end, [](uint8_t v) { return v != 0; }) == end;
}

template <size_t Size>
std::string ExtractString(const char (&start)[Size]) {
  const char* end = start + Size;
  end = std::find(start, end, 0);
  return std::string(start, end);
}

}  // namespace

TarTraceReader::TarTraceReader(TraceProcessorContext* context)
    : context_(context) {}

TarTraceReader::~TarTraceReader() = default;

util::Status TarTraceReader::Parse(TraceBlobView blob) {
  ParseResult result = ParseResult::kOk;
  buffer_.PushBack(std::move(blob));
  while (!buffer_.empty() && result == ParseResult::kOk) {
    switch (state_) {
      case State::kMetadata:
      case State::kZeroMetadata: {
        ASSIGN_OR_RETURN(result, ParseMetadata());
        break;
      }
      case State::kContent: {
        ASSIGN_OR_RETURN(result, ParseContent());
        break;
      }
      case State::kDone:
        // We are done, ignore any more data
        buffer_.PopFrontUntil(buffer_.end_offset());
    }
  }
  return base::OkStatus();
}

base::Status TarTraceReader::NotifyEndOfFile() {
  if (state_ != State::kDone) {
    return base::ErrStatus("Premature end of TAR file");
  }

  for (auto& file : ordered_files_) {
    auto chunk_reader =
        std::make_unique<ForwardingTraceParser>(context_, file.second.id);
    auto& parser = *chunk_reader;
    context_->chunk_readers.push_back(std::move(chunk_reader));

    for (auto& data : file.second.data) {
      RETURN_IF_ERROR(parser.Parse(std::move(data)));
    }
    RETURN_IF_ERROR(parser.NotifyEndOfFile());
    // Make sure the ForwardingTraceParser determined the same trace type as we
    // did.
    PERFETTO_CHECK(parser.trace_type() == file.first.trace_type);
  }

  return base::OkStatus();
}

base::StatusOr<TarTraceReader::ParseResult> TarTraceReader::ParseMetadata() {
  PERFETTO_CHECK(!metadata_.has_value());
  auto blob = buffer_.SliceOff(buffer_.start_offset(), kHeaderSize);
  if (!blob) {
    return ParseResult::kNeedsMoreData;
  }
  buffer_.PopFrontBytes(kHeaderSize);
  const Header& header = *reinterpret_cast<const Header*>(blob->data());

  TarType type = header.GetTarFileType();

  if (type == TarType::kUnknown) {
    if (!IsAllZeros(*blob)) {
      return base::ErrStatus("Invalid magic value");
    }
    // EOF is signaled by two consecutive zero headers.
    if (state_ == State::kMetadata) {
      // Fist time we see all zeros. NExt parser loop will enter ParseMetadata
      // again and decide whether it is the real end or maybe a ral header
      // comes.
      state_ = State::kZeroMetadata;
    } else {
      // Previous header was zeros, thus we are done.
      PERFETTO_CHECK(state_ == State::kZeroMetadata);
      state_ = State::kDone;
    }
    return ParseResult::kOk;
  }

  if (type == TarType::kUstar && (header.magic.ustar.version[0] != '0' ||
                                  header.magic.ustar.version[1] != '0')) {
    return base::ErrStatus("Invalid version: %c%c",
                           header.magic.ustar.version[0],
                           header.magic.ustar.version[1]);
  }

  std::optional<uint64_t> size = ExtractUint64(header.size);
  if (!size.has_value()) {
    return base::ErrStatus("Failed to parse octal size field.");
  }

  metadata_.emplace();
  metadata_->size = *size;
  metadata_->type_flag = *header.type_flag;

  if (long_name_) {
    metadata_->name = std::move(*long_name_);
    long_name_.reset();
  } else {
    metadata_->name =
        ExtractString(header.prefix) + "/" + ExtractString(header.name);
  }

  switch (metadata_->type_flag) {
    case TYPE_FLAG_REGULAR:
    case TYPE_FLAG_AREGULAR:
    case TYPE_FLAG_GNU_LONG_NAME:
      state_ = State::kContent;
      break;

    default:
      if (metadata_->size != 0) {
        return base::ErrStatus("Unsupported file type: 0x%02x",
                               metadata_->type_flag);
      }
      state_ = State::kMetadata;
      break;
  }

  return ParseResult::kOk;
}

base::StatusOr<TarTraceReader::ParseResult> TarTraceReader::ParseContent() {
  PERFETTO_CHECK(metadata_.has_value());

  size_t data_and_padding_size = base::AlignUp(metadata_->size, kHeaderSize);
  if (buffer_.avail() < data_and_padding_size) {
    return ParseResult::kNeedsMoreData;
  }

  if (metadata_->type_flag == TYPE_FLAG_GNU_LONG_NAME) {
    TraceBlobView data =
        *buffer_.SliceOff(buffer_.start_offset(), metadata_->size);
    long_name_ = std::string(reinterpret_cast<const char*>(data.data()),
                             metadata_->size);
  } else {
    AddFile(*metadata_,
            *buffer_.SliceOff(
                buffer_.start_offset(),
                std::min(static_cast<uint64_t>(512), metadata_->size)),
            buffer_.MultiSliceOff(buffer_.start_offset(), metadata_->size));
  }

  buffer_.PopFrontBytes(data_and_padding_size);

  metadata_.reset();
  state_ = State::kMetadata;
  return ParseResult::kOk;
}

void TarTraceReader::AddFile(const Metadata& metadata,
                             TraceBlobView header,
                             std::vector<TraceBlobView> data) {
  auto file_id = context_->trace_file_tracker->AddFile(metadata.name);
  context_->trace_file_tracker->SetSize(file_id, metadata.size);
  ordered_files_.emplace(
      ArchiveEntry{metadata.name, ordered_files_.size(),
                   GuessTraceType(header.data(), header.size())},
      File{file_id, std::move(data)});
}

}  // namespace perfetto::trace_processor
