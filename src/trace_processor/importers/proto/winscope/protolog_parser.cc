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

#include "src/trace_processor/importers/proto/winscope/protolog_parser.h"
#include "src/trace_processor/importers/proto/winscope/protolog_messages_tracker.h"

#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/trace/android/protolog.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/winscope/winscope.descriptor.h"
#include "src/trace_processor/importers/proto/winscope/winscope_args_parser.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include <sstream>
namespace perfetto {
namespace trace_processor {

enum ProtoLogLevel : int32_t {
  DEBUG = 1,
  VERBOSE = 2,
  INFO = 3,
  WARN = 4,
  ERROR = 5,
  WTF = 6,
};

ProtoLogParser::ProtoLogParser(TraceProcessorContext* context)
    : context_(context),
      args_parser_{pool_},
      log_level_debug_string_id_(context->storage->InternString("DEBUG")),
      log_level_verbose_string_id_(context->storage->InternString("VERBOSE")),
      log_level_info_string_id_(context->storage->InternString("INFO")),
      log_level_warn_string_id_(context->storage->InternString("WARN")),
      log_level_error_string_id_(context->storage->InternString("ERROR")),
      log_level_wtf_string_id_(context->storage->InternString("WTF")),
      log_level_unknown_string_id_(context_->storage->InternString("UNKNOWN")) {
  pool_.AddFromFileDescriptorSet(kWinscopeDescriptor.data(),
                                 kWinscopeDescriptor.size());
}

void ProtoLogParser::ParseProtoLogMessage(
    PacketSequenceStateGeneration* sequence_state,
    protozero::ConstBytes blob,
    int64_t timestamp) {
  protos::pbzero::ProtoLogMessage::Decoder protolog_message(blob);

  std::vector<int64_t> sint64_params;
  for (auto it = protolog_message.sint64_params(); it; ++it) {
    sint64_params.emplace_back(*it);
  }

  std::vector<double> double_params;
  for (auto it = protolog_message.double_params(); it; ++it) {
    double_params.emplace_back(*it);
  }

  std::vector<bool> boolean_params;
  for (auto it = protolog_message.boolean_params(); it; ++it) {
    boolean_params.emplace_back(*it);
  }

  std::vector<std::string> string_params;
  if (protolog_message.has_str_param_iids()) {
    if (sequence_state->state()->IsIncrementalStateValid()) {
      for (auto it = protolog_message.str_param_iids(); it; ++it) {
        auto decoder =
            sequence_state->state()
                ->current_generation()
                ->LookupInternedMessage<protos::pbzero::InternedData::
                                            kProtologStringArgsFieldNumber,
                                        protos::pbzero::InternedString>(
                    it.field().as_uint32());

        if (!decoder) {
          // This shouldn't happen since we already checked the incremental
          // state is valid.
          string_params.emplace_back("<ERROR>");
          context_->storage->IncrementStats(
              stats::winscope_protolog_missing_interned_arg_parse_errors);
          continue;
        }

        string_params.emplace_back(decoder->str().ToStdString());
      }
    } else {
      // If the incremental state is not valid we will not be able to decode
      // the interned strings correctly with 100% certainty so we will provide
      // string parameters that are not decoded.
      string_params.emplace_back("<MISSING_STR_ARG>");
    }
  }

  std::optional<StringId> stacktrace = std::nullopt;
  if (protolog_message.has_stacktrace_iid()) {
    auto stacktrace_decoder =
        sequence_state->state()
            ->current_generation()
            ->LookupInternedMessage<
                protos::pbzero::InternedData::kProtologStacktraceFieldNumber,
                protos::pbzero::InternedString>(
                protolog_message.stacktrace_iid());

    if (!stacktrace_decoder) {
      // This shouldn't happen since we already checked the incremental
      // state is valid.
      string_params.emplace_back("<ERROR>");
      context_->storage->IncrementStats(
          stats::winscope_protolog_missing_interned_stacktrace_parse_errors);
    } else {
      stacktrace = context_->storage->InternString(
          base::StringView(stacktrace_decoder->str().ToStdString()));
    }
  }

  auto* protolog_table = context_->storage->mutable_protolog_table();

  tables::ProtoLogTable::Row row;
  auto row_id = protolog_table->Insert(row).id;

  auto protolog_message_tracker =
      ProtoLogMessagesTracker::GetOrCreate(context_);
  struct ProtoLogMessagesTracker::TrackedProtoLogMessage tracked_message = {
      protolog_message.message_id(),
      std::move(sint64_params),
      std::move(double_params),
      std::move(boolean_params),
      std::move(string_params),
      stacktrace,
      row_id,
      timestamp};
  protolog_message_tracker->TrackMessage(std::move(tracked_message));
}

void ProtoLogParser::ParseProtoLogViewerConfig(protozero::ConstBytes blob) {
  auto* protolog_table = context_->storage->mutable_protolog_table();

  protos::pbzero::ProtoLogViewerConfig::Decoder protolog_viewer_config(blob);

  std::unordered_map<uint32_t, std::string> group_tags;
  for (auto it = protolog_viewer_config.groups(); it; ++it) {
    protos::pbzero::ProtoLogViewerConfig::Group::Decoder group(*it);
    group_tags.insert({group.id(), group.tag().ToStdString()});
  }

  auto protolog_message_tracker =
      ProtoLogMessagesTracker::GetOrCreate(context_);

  for (auto it = protolog_viewer_config.messages(); it; ++it) {
    protos::pbzero::ProtoLogViewerConfig::MessageData::Decoder message_data(
        *it);

    auto tracked_messages_opt =
        protolog_message_tracker->GetTrackedMessagesByMessageId(
            message_data.message_id());

    if (tracked_messages_opt.has_value()) {
      auto group_tag = group_tags.find(message_data.group_id())->second;

      for (const auto& tracked_message : *tracked_messages_opt.value()) {
        auto formatted_message = FormatMessage(
            message_data.message().ToStdString(), tracked_message.sint64_params,
            tracked_message.double_params, tracked_message.boolean_params,
            tracked_message.string_params);

        auto row =
            protolog_table->FindById(tracked_message.table_row_id).value();

        row.set_ts(tracked_message.timestamp);

        StringPool::Id level;
        switch (message_data.level()) {
          case ProtoLogLevel::DEBUG:
            level = log_level_debug_string_id_;
            break;
          case ProtoLogLevel::VERBOSE:
            level = log_level_verbose_string_id_;
            break;
          case ProtoLogLevel::INFO:
            level = log_level_info_string_id_;
            break;
          case ProtoLogLevel::WARN:
            level = log_level_warn_string_id_;
            break;
          case ProtoLogLevel::ERROR:
            level = log_level_error_string_id_;
            break;
          case ProtoLogLevel::WTF:
            level = log_level_wtf_string_id_;
            break;
          default:
            level = log_level_unknown_string_id_;
            break;
        }
        row.set_level(level);

        auto tag = context_->storage->InternString(base::StringView(group_tag));
        row.set_tag(tag);

        auto message = context_->storage->InternString(
            base::StringView(formatted_message));
        row.set_message(message);

        if (tracked_message.stacktrace.has_value()) {
          row.set_stacktrace(tracked_message.stacktrace.value());
        }
      }
    }
  }
}

std::string ProtoLogParser::FormatMessage(
    const std::string message,
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
    if (message.at(i) == '%') {
      switch (message.at(i + 1)) {
        case '%':
          break;
        case 'd': {
          base::StackString<32> param("%" PRId64, *sint64_params_itr);
          formatted_message.append(param.c_str());
          ++sint64_params_itr;
          break;
        }
        case 'o': {
          base::StackString<32> param("%" PRIo64, *sint64_params_itr);
          formatted_message.append(param.c_str());
          ++sint64_params_itr;
          break;
        }
        case 'x': {
          base::StackString<32> param("%" PRIx64, *sint64_params_itr);
          formatted_message.append(param.c_str());
          ++sint64_params_itr;
          break;
        }
        case 'f': {
          base::StackString<32> param("%f", *double_params_itr);
          formatted_message.append(param.c_str());
          ++double_params_itr;
          break;
        }
        case 'e': {
          base::StackString<32> param("%e", *double_params_itr);
          formatted_message.append(param.c_str());
          ++double_params_itr;
          break;
        }
        case 'g': {
          base::StackString<32> param("%g", *double_params_itr);
          formatted_message.append(param.c_str());
          ++double_params_itr;
          break;
        }
        case 's': {
          formatted_message.append(str_params_itr->c_str());
          ++str_params_itr;
          break;
          case 'b':
            formatted_message.append(*boolean_params_itr ? "true" : "false");
            ++boolean_params_itr;
            break;
        }
        default:
          // Should never happen
          context_->storage->IncrementStats(
              stats::winscope_protolog_invalid_interpolation_parse_errors);
      }

      i += 2;
    } else {
      formatted_message.push_back(message[i]);
      i += 1;
    }
  }

  return formatted_message;
}

}  // namespace trace_processor
}  // namespace perfetto
