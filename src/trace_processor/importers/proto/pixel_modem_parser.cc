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
#include "src/trace_processor/importers/proto/pigweed_detokenizer.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {

namespace {

constexpr std::string_view kKeyDelimiterStart = "\u25A0";
constexpr std::string_view kKeyDelimiterEnd = "\u2666";
constexpr std::string_view kKeyDomain = "domain";
constexpr std::string_view kKeyFormat = "format";
constexpr std::string_view kModemNamePrefix = "Pixel Modem Events: ";
constexpr std::string_view kModemName = "Pixel Modem Events";

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
    : context_(context),
      detokenizer_(pigweed::CreateNullDetokenizer()),
      template_id_(context->storage->InternString("raw_template")),
      token_id_(context->storage->InternString("token_id")),
      token_id_hex_(context->storage->InternString("token_id_hex")),
      packet_timestamp_id_(context->storage->InternString("packet_ts")) {}

PixelModemParser::~PixelModemParser() = default;

base::Status PixelModemParser::SetDatabase(protozero::ConstBytes blob) {
  ASSIGN_OR_RETURN(detokenizer_, pigweed::CreateDetokenizer(blob));
  return base::OkStatus();
}

base::Status PixelModemParser::ParseEvent(int64_t ts,
                                          uint64_t trace_packet_ts,
                                          protozero::ConstBytes blob) {
  ASSIGN_OR_RETURN(pigweed::DetokenizedString detokenized_str,
                   detokenizer_.Detokenize(blob));

  std::string event = detokenized_str.Format();

  auto map = SplitUpModemString(event);
  auto domain = map.find(std::string(kKeyDomain));
  auto format = map.find(std::string(kKeyFormat));

  std::string track_name = domain == map.end()
                               ? std::string(kModemName)
                               : std::string(kModemNamePrefix) + domain->second;
  std::string slice_name = format == map.end() ? event : format->second;

  StringId track_name_id = context_->storage->InternString(track_name.c_str());
  StringId slice_name_id = context_->storage->InternString(slice_name.c_str());
  auto set_id =
      context_->async_track_set_tracker->InternGlobalTrackSet(track_name_id);
  TrackId id = context_->async_track_set_tracker->Scoped(set_id, ts, 0);

  context_->slice_tracker->Scoped(
      ts, id, kNullStringId, slice_name_id, 0,
      [this, &detokenized_str,
       trace_packet_ts](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(template_id_,
                         Variadic::String(context_->storage->InternString(
                             detokenized_str.template_str().c_str())));
        uint32_t token = detokenized_str.token();
        inserter->AddArg(token_id_, Variadic::Integer(token));
        inserter->AddArg(token_id_hex_,
                         Variadic::String(context_->storage->InternString(
                             base::IntToHexString(token).c_str())));
        inserter->AddArg(packet_timestamp_id_,
                         Variadic::UnsignedInteger(trace_packet_ts));
        auto pw_args = detokenized_str.args();
        for (size_t i = 0; i < pw_args.size(); i++) {
          StringId arg_name = context_->storage->InternString(
              ("pw_token_" + std::to_string(token) + ".arg_" +
               std::to_string(i))
                  .c_str());
          auto arg = pw_args[i];
          if (int64_t* int_arg = std::get_if<int64_t>(&arg)) {
            inserter->AddArg(arg_name, Variadic::Integer(*int_arg));
          } else if (uint64_t* uint_arg = std::get_if<uint64_t>(&arg)) {
            inserter->AddArg(arg_name, Variadic::UnsignedInteger(*uint_arg));
          } else {
            inserter->AddArg(arg_name, Variadic::Real(std::get<double>(arg)));
          }
        }
      });

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
