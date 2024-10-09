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

#include "src/trace_processor/importers/art_method/art_method_tokenizer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_method/art_method_event.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

namespace perfetto::trace_processor::art_method {
namespace {

constexpr uint32_t kTraceMagic = 0x574f4c53;  // 'SLOW'

std::string_view ToStringView(const TraceBlobView& tbv) {
  return {reinterpret_cast<const char*>(tbv.data()), tbv.size()};
}

std::string ConstructPathname(const std::string& class_name,
                              const std::string& pathname) {
  size_t index = class_name.rfind('/');
  if (index != std::string::npos && base::EndsWith(pathname, ".java")) {
    return class_name.substr(0, index + 1) + pathname;
  }
  return pathname;
}

uint64_t ToLong(const TraceBlobView& tbv) {
  uint64_t x = 0;
  memcpy(base::AssumeLittleEndian(&x), tbv.data(), tbv.size());
  return x;
}

uint32_t ToInt(const TraceBlobView& tbv) {
  uint32_t x = 0;
  memcpy(base::AssumeLittleEndian(&x), tbv.data(), tbv.size());
  return x;
}

uint16_t ToShort(const TraceBlobView& tbv) {
  uint16_t x = 0;
  memcpy(base::AssumeLittleEndian(&x), tbv.data(), tbv.size());
  return x;
}

}  // namespace

ArtMethodTokenizer::ArtMethodTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}
ArtMethodTokenizer::~ArtMethodTokenizer() = default;

base::Status ArtMethodTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));
  auto it = reader_.GetIterator();
  for (bool cnt = true; cnt;) {
    switch (mode_) {
      case kHeaderDetection: {
        ASSIGN_OR_RETURN(cnt, ParseHeaderDetection(it));
        break;
      }
      case kHeaderVersion: {
        ASSIGN_OR_RETURN(cnt, ParseHeaderVersion(it));
        break;
      }
      case kHeaderOptions: {
        ASSIGN_OR_RETURN(cnt, ParseHeaderOptions(it));
        break;
      }
      case kHeaderThreads: {
        ASSIGN_OR_RETURN(cnt, ParseHeaderThreads(it));
        break;
      }
      case kHeaderMethods: {
        ASSIGN_OR_RETURN(cnt, ParseHeaderMethods(it));
        break;
      }
      case kDataHeader: {
        ASSIGN_OR_RETURN(cnt, ParseDataHeader(it));
        break;
      }
      case kData: {
        size_t s = it.file_offset();
        for (size_t i = s;; i += record_size_) {
          auto record = reader_.SliceOff(i, record_size_);
          if (!record) {
            PERFETTO_CHECK(it.MaybeAdvance(i - s));
            cnt = false;
            break;
          }

          ArtMethodEvent evt{};
          evt.tid = version_ == 1 ? record->data()[0]
                                  : ToShort(record->slice_off(0, 2));
          uint32_t methodid_action = ToInt(record->slice_off(2, 4));
          uint32_t ts_delta = clock_ == kDual ? ToInt(record->slice_off(10, 4))
                                              : ToInt(record->slice_off(6, 4));

          uint32_t action = methodid_action & 0x03;
          uint32_t method_id = methodid_action & ~0x03u;

          const auto& m = method_map_[method_id];
          evt.method = m.name;
          evt.pathname = m.pathname;
          evt.line_number = m.line_number;
          switch (action) {
            case 0:
              evt.action = ArtMethodEvent::kEnter;
              break;
            case 1:
            case 2:
              evt.action = ArtMethodEvent::kExit;
              break;
          }
          ASSIGN_OR_RETURN(int64_t ts,
                           context_->clock_tracker->ToTraceTime(
                               protos::pbzero::BUILTIN_CLOCK_MONOTONIC,
                               (ts_ + ts_delta) * 1000));
          context_->sorter->PushArtMethodEvent(ts, evt);
        }
        break;
      }
    }
  }
  reader_.PopFrontUntil(it.file_offset());
  return base::OkStatus();
}

base::StatusOr<bool> ArtMethodTokenizer::ParseHeaderDetection(Iterator& it) {
  auto smagic = reader_.SliceOff(it.file_offset(), 4);
  if (!smagic) {
    return false;
  }
  uint32_t magic = ToInt(*smagic);
  if (magic == kTraceMagic) {
    return base::ErrStatus(
        "ART Method trace is in streaming format: this is not supported");
  }
  auto raw = it.MaybeFindAndAdvance('\n');
  if (!raw) {
    return false;
  }
  context_->clock_tracker->SetTraceTimeClock(
      protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
  RETURN_IF_ERROR(ParseHeaderSectionLine(ToStringView(*raw)));
  return true;
}

base::StatusOr<bool> ArtMethodTokenizer::ParseHeaderVersion(Iterator& it) {
  auto line = it.MaybeFindAndAdvance('\n');
  if (!line) {
    return false;
  }
  std::string version_str(ToStringView(*line));
  auto version = base::StringToInt32(version_str);
  if (!version || *version < 1 || *version > 3) {
    return base::ErrStatus("ART Method trace: trace version (%s) not supported",
                           version_str.c_str());
  }
  version_ = static_cast<uint32_t>(*version);
  mode_ = kHeaderOptions;
  return true;
}

base::StatusOr<bool> ArtMethodTokenizer::ParseHeaderOptions(Iterator& it) {
  for (auto r = it.MaybeFindAndAdvance('\n'); r;
       r = it.MaybeFindAndAdvance('\n')) {
    std::string_view l = ToStringView(*r);
    if (l[0] == '*') {
      RETURN_IF_ERROR(ParseHeaderSectionLine(l));
      return true;
    }
    auto res = base::SplitString(std::string(l), "=");
    if (res.size() != 2) {
      return base::ErrStatus("ART method tracing: unable to parse option");
    }
    if (res[0] == "clock") {
      if (res[1] == "dual") {
        clock_ = kDual;
      } else if (res[1] == "wall") {
        clock_ = kWall;
      } else if (res[1] == "thread-cpu") {
        return base::ErrStatus(
            "ART method tracing: thread-cpu clock is *not* supported. Use wall "
            "or dual clocks");
      } else {
        return base::ErrStatus("ART method tracing: unknown clock %s",
                               res[1].c_str());
      }
    }
  }
  return false;
}

base::StatusOr<bool> ArtMethodTokenizer::ParseHeaderThreads(Iterator& it) {
  for (auto r = it.MaybeFindAndAdvance('\n'); r;
       r = it.MaybeFindAndAdvance('\n')) {
    std::string_view l = ToStringView(*r);
    if (l[0] == '*') {
      RETURN_IF_ERROR(ParseHeaderSectionLine(l));
      return true;
    }
  }
  return false;
}

base::StatusOr<bool> ArtMethodTokenizer::ParseHeaderMethods(Iterator& it) {
  for (auto r = it.MaybeFindAndAdvance('\n'); r;
       r = it.MaybeFindAndAdvance('\n')) {
    std::string_view l = ToStringView(*r);
    if (l[0] == '*') {
      RETURN_IF_ERROR(ParseHeaderSectionLine(l));
      return true;
    }
    auto tokens = base::SplitString(std::string(l), "\t");
    auto id = base::StringToUInt32(tokens[0], 16);
    if (!id) {
      return base::ErrStatus(
          "ART method trace: unable to parse method id as integer: %s",
          tokens[0].c_str());
    }

    std::string class_name = tokens[1];
    std::string method_name;
    std::string signature;
    std::optional<StringId> pathname;
    std::optional<uint32_t> line_number;
    if (tokens.size() == 6) {
      method_name = tokens[2];
      signature = tokens[3];
      pathname = context_->storage->InternString(
          base::StringView(ConstructPathname(class_name, tokens[4])));
      line_number = base::StringToUInt32(tokens[5]);
    } else if (tokens.size() > 2) {
      if (base::StartsWith(tokens[3], "(")) {
        method_name = tokens[2];
        signature = tokens[3];
        if (tokens.size() >= 5) {
          pathname =
              context_->storage->InternString(base::StringView(tokens[4]));
        }
      } else {
        pathname = context_->storage->InternString(base::StringView(tokens[2]));
        line_number = base::StringToUInt32(tokens[3]);
      }
    }
    base::StackString<2048> slice_name("%s.%s: %s", class_name.c_str(),
                                       method_name.c_str(), signature.c_str());
    method_map_[*id] = {
        context_->storage->InternString(slice_name.string_view()),
        pathname,
        line_number,
    };
  }
  return false;
}

base::StatusOr<bool> ArtMethodTokenizer::ParseDataHeader(Iterator& it) {
  size_t begin = it.file_offset();
  if (!it.MaybeAdvance(32)) {
    return false;
  }
  auto header = reader_.SliceOff(begin, it.file_offset() - begin);
  uint32_t magic = ToInt(header->slice_off(0, 4));
  if (magic != kTraceMagic) {
    return base::ErrStatus("ART Method trace: expected pre-data magic");
  }
  uint16_t version = ToShort(header->slice_off(4, 2));
  if (version != version_) {
    return base::ErrStatus(
        "ART Method trace: trace version does not match data version");
  }
  ts_ = static_cast<int64_t>(ToLong(header->slice_off(8, 8)));
  switch (version_) {
    case 1:
      record_size_ = 9;
      break;
    case 2:
      record_size_ = 10;
      break;
    case 3:
      record_size_ = ToShort(header->slice_off(16, 2));
      break;
    default:
      PERFETTO_FATAL("Illegal version %u", version_);
  }
  mode_ = kData;
  return true;
}

base::Status ArtMethodTokenizer::ParseHeaderSectionLine(std::string_view line) {
  if (line == "*version") {
    mode_ = kHeaderVersion;
    return base::OkStatus();
  }
  if (line == "*threads") {
    mode_ = kHeaderThreads;
    return base::OkStatus();
  }
  if (line == "*methods") {
    mode_ = kHeaderMethods;
    return base::OkStatus();
  }
  if (line == "*end") {
    mode_ = kDataHeader;
    return base::OkStatus();
  }
  return base::ErrStatus(
      "ART Method trace: unexpected line (%s) when expecting section header "
      "(line starting with *)",
      std::string(line).c_str());
}

base::Status ArtMethodTokenizer::NotifyEndOfFile() {
  // DNS: also add a check here for whether our state machine reached the end
  // too.
  if (!reader_.empty() || mode_ != kData) {
    return base::ErrStatus("ART Method trace: trace is incomplete");
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::art_method
