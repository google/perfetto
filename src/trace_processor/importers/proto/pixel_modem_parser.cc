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

#include "src/trace_processor/importers/proto/pixel_modem_parser.h"

#include <cstddef>
#include <cstdint>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/token_database.h"

namespace perfetto::trace_processor {

namespace {

constexpr std::string_view kKeyDelimiterStart = "\u25A0";
constexpr std::string_view kKeyDelimiterEnd = "\u2666";
constexpr std::string_view kKeyDomain = "domain";
constexpr std::string_view kKeyFormat = "format";
constexpr std::string_view kModemNamePrefix = "Pixel Modem Events: ";
constexpr std::string_view kModemName = "Pixel Modem Events";

struct PigweedConstBytes {
  const uint8_t* data_;
  size_t size_;

  const uint8_t* begin() const { return data_; }
  const uint8_t* end() const { return data_ + size_; }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  uint8_t operator[](size_t i) const { return data_[i]; }
};

// Modem inputs in particular have this key-value encoding. It's not a Pigweed
// thing.
std::map<std::string, std::string> SplitUpModemString(std::string input) {
  auto delimStart = std::string(kKeyDelimiterStart);
  auto delimEnd = std::string(kKeyDelimiterEnd);

  std::map<std::string, std::string> result;

  std::vector<std::string> pairs = base::SplitString(input, delimStart);
  for (auto it = pairs.begin(); it != pairs.end(); it++) {
    std::vector<std::string> pair = base::SplitString(*it, delimEnd);
    if (pair.size() >= 2) {
      result.insert({pair[0], pair[1]});
    }
  }

  return result;
}
}  // namespace

PixelModemParser::PixelModemParser(TraceProcessorContext* context)
    : context_(context) {}

PixelModemParser::~PixelModemParser() = default;

void PixelModemParser::SetDatabase(protozero::ConstBytes blob) {
  detokenizer_ = std::make_unique<pw::tokenizer::Detokenizer>(
      pw::tokenizer::TokenDatabase::Create(
          PigweedConstBytes{blob.data, blob.size}));
}

void PixelModemParser::ParseEvent(int64_t ts, protozero::ConstBytes blob) {
  pw::tokenizer::DetokenizedString detokenized_str =
      detokenizer_->Detokenize(PigweedConstBytes{blob.data, blob.size});
  if (!detokenized_str.ok()) {
    return;
  }
  std::string event = detokenized_str.BestString();

  auto map = SplitUpModemString(event);
  auto domain = map.find(std::string(kKeyDomain));
  auto format = map.find(std::string(kKeyFormat));

  std::string track_name = domain == map.end()
                               ? std::string(kModemName)
                               : std::string(kModemNamePrefix) + domain->second;
  std::string slice_name = format == map.end() ? event : format->second;

  StringId track_name_id = context_->storage->InternString(track_name.c_str());
  auto set_id =
      context_->async_track_set_tracker->InternGlobalTrackSet(track_name_id);
  TrackId id = context_->async_track_set_tracker->Begin(set_id, 0);
  StringId slice_name_id = context_->storage->InternString(slice_name.c_str());
  context_->slice_tracker->Begin(ts, id, kNullStringId, slice_name_id);
  context_->slice_tracker->End(ts, id);
  context_->async_track_set_tracker->End(set_id, 0);
}

}  // namespace perfetto::trace_processor
