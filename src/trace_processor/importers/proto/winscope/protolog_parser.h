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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_PROTOLOG_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_PROTOLOG_PARSER_H_

#include "protos/perfetto/trace/android/protolog.pbzero.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

namespace perfetto {

namespace trace_processor {

class TraceProcessorContext;

class ProtoLogParser {
 public:
  explicit ProtoLogParser(TraceProcessorContext*);
  void ParseProtoLogMessage(PacketSequenceStateGeneration* sequence_state,
                            protozero::ConstBytes,
                            int64_t timestamp);
  void ParseProtoLogViewerConfig(protozero::ConstBytes);

 private:
  std::string FormatMessage(const std::string message,
                            const std::vector<int64_t>& sint64_params,
                            const std::vector<double>& double_params,
                            const std::vector<bool>& boolean_params,
                            const std::vector<std::string>& string_params);

  static constexpr auto* kProtoLogMessageProtoName =
      "perfetto.protos.ProtoLogMessage";

  TraceProcessorContext* const context_;
  DescriptorPool pool_;
  util::ProtoToArgsParser args_parser_;

  const StringId log_level_debug_string_id_;
  const StringId log_level_verbose_string_id_;
  const StringId log_level_info_string_id_;
  const StringId log_level_warn_string_id_;
  const StringId log_level_error_string_id_;
  const StringId log_level_wtf_string_id_;
  const StringId log_level_unknown_string_id_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_PROTOLOG_PARSER_H_
