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

#include "src/trace_processor/importers/proto/winscope/protolog_message_decoder.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto::trace_processor::winscope {

ProtoLogMessageDecoder::ProtoLogMessageDecoder(TraceProcessorContext* context)
    : context_(context) {}

std::optional<DecodedMessage> ProtoLogMessageDecoder::Decode(
    uint64_t message_id,
    const std::vector<int64_t>& sint64_params,
    const std::vector<double>& double_params,
    const std::vector<bool>& boolean_params,
    const std::vector<std::string>& string_params) {
  auto* tracked_messages_vec_ptr = tracked_messages_.Find(message_id);
  if (tracked_messages_vec_ptr == nullptr) {
    return std::nullopt;
  }

  const std::vector<TrackedMessage>& messages = *tracked_messages_vec_ptr;

  if (messages.size() == 1) {
    const auto& tracked_message = messages[0];
    auto message = tracked_message.message;

    auto group = tracked_groups_.Find(tracked_message.group_id);
    if (group == nullptr) {
      return std::nullopt;
    }

    auto formatted_message = FormatMessage(
        message, sint64_params, double_params, boolean_params, string_params);
    return DecodedMessage{tracked_message.level, group->tag, formatted_message,
                          tracked_message.location};
  } else {
    std::string collision_indicator = "<PROTOLOG COLLISION (id=0x" +
                                      base::Uint64ToHexString(message_id) +
                                      "): ";
    for (size_t i = 0; i < messages.size(); ++i) {
      auto formatted_message =
          FormatMessage(messages[i].message, sint64_params, double_params,
                        boolean_params, string_params);
      collision_indicator += "'" + formatted_message + "'";
      if (i < messages.size() - 1) {
        collision_indicator += "\n ";
      }
    }
    collision_indicator += "'>";

    return DecodedMessage{ProtoLogLevel::WARN, nullptr, collision_indicator,
                          std::nullopt};
  }
}

void ProtoLogMessageDecoder::TrackGroup(uint32_t id, const std::string& tag) {
  auto tracked_group = tracked_groups_.Find(id);
  if (tracked_group != nullptr && tracked_group->tag != tag) {
    context_->storage->IncrementStats(
        stats::winscope_protolog_view_config_collision);
  }
  tracked_groups_.Insert(id, TrackedGroup{tag});
}

void ProtoLogMessageDecoder::TrackMessage(
    uint64_t message_id,
    ProtoLogLevel level,
    uint32_t group_id,
    const std::string& message,
    const std::optional<std::string>& location) {
  TrackedMessage new_tracked_message{level, group_id, message, location};

  auto* existing_messages_ptr = tracked_messages_.Find(message_id);

  if (existing_messages_ptr == nullptr) {
    tracked_messages_.Insert(message_id,
                             std::vector<TrackedMessage>{new_tracked_message});
    return;
  }

  std::vector<TrackedMessage>& existing_messages = *existing_messages_ptr;
  bool message_already_tracked = false;

  for (const auto& existing_msg : existing_messages) {
    if (existing_msg.message == new_tracked_message.message &&
        existing_msg.level == new_tracked_message.level &&
        existing_msg.group_id == new_tracked_message.group_id) {
      message_already_tracked = true;
      break;
    }
  }

  if (!message_already_tracked) {
    context_->storage->IncrementStats(
        stats::winscope_protolog_view_config_collision);
    existing_messages.push_back(new_tracked_message);
  }
}

std::string ProtoLogMessageDecoder::FormatMessage(
    const std::string& message,
    const std::vector<int64_t>& sint64_params,
    const std::vector<double>& double_params,
    const std::vector<bool>& boolean_params,
    const std::vector<std::string>& string_params) {
  std::string formatted_message;
  formatted_message.reserve(message.size());

  auto sint64_params_itr = sint64_params.begin();
  auto double_params_itr = double_params.begin();
  auto boolean_params_itr = boolean_params.begin();
  auto str_params_itr = string_params.begin();

  for (size_t i = 0; i < message.length();) {
    if (message.at(i) == '%' && i + 1 < message.length()) {
      switch (message.at(i + 1)) {
        case '%':
          formatted_message.push_back('%');
          break;
        case 'd': {
          if (sint64_params_itr == sint64_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          base::StackString<32> param("%" PRId64, *sint64_params_itr);
          formatted_message.append(param.c_str());
          ++sint64_params_itr;
          break;
        }
        case 'o': {
          if (sint64_params_itr == sint64_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          base::StackString<32> param(
              "%" PRIo64, static_cast<uint64_t>(*sint64_params_itr));
          formatted_message.append(param.c_str());
          ++sint64_params_itr;
          break;
        }
        case 'x': {
          if (sint64_params_itr == sint64_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          base::StackString<32> param(
              "%" PRIx64, static_cast<uint64_t>(*sint64_params_itr));
          formatted_message.append(param.c_str());
          ++sint64_params_itr;
          break;
        }
        case 'f': {
          if (double_params_itr == double_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          base::StackString<32> param("%f", *double_params_itr);
          formatted_message.append(param.c_str());
          ++double_params_itr;
          break;
        }
        case 'e': {
          if (double_params_itr == double_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          base::StackString<32> param("%e", *double_params_itr);
          formatted_message.append(param.c_str());
          ++double_params_itr;
          break;
        }
        case 'g': {
          if (double_params_itr == double_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          base::StackString<32> param("%g", *double_params_itr);
          formatted_message.append(param.c_str());
          ++double_params_itr;
          break;
        }
        case 's': {
          if (str_params_itr == string_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          formatted_message.append(*str_params_itr);
          ++str_params_itr;
          break;
        }
        case 'b': {
          if (boolean_params_itr == boolean_params.end()) {
            context_->storage->IncrementStats(
                stats::winscope_protolog_param_mismatch);
            formatted_message.append("[MISSING_PARAM]");
            break;
          }
          formatted_message.append(*boolean_params_itr ? "true" : "false");
          ++boolean_params_itr;
          break;
        }
        default:
          formatted_message.push_back(message[i]);
          formatted_message.push_back(message[i + 1]);
      }

      i += 2;
    } else {
      formatted_message.push_back(message[i]);
      i += 1;
    }
  }

  if (sint64_params_itr != sint64_params.end() ||
      double_params_itr != double_params.end() ||
      boolean_params_itr != boolean_params.end() ||
      str_params_itr != string_params.end()) {
    context_->storage->IncrementStats(stats::winscope_protolog_param_mismatch);
  }

  return formatted_message;
}

}  // namespace perfetto::trace_processor::winscope
