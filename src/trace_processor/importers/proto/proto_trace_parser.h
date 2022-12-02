/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_PARSER_H_

#include <stdint.h>

#include <array>
#include <memory>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {

namespace protos {
namespace pbzero {
class TracePacket_Decoder;
}  // namespace pbzero
}  // namespace protos

namespace trace_processor {

class PacketSequenceState;
class TraceProcessorContext;

class ProtoTraceParser : public TraceParser {
 public:
  using ConstBytes = protozero::ConstBytes;
  explicit ProtoTraceParser(TraceProcessorContext*);
  ~ProtoTraceParser() override;

  void ParseTrackEvent(int64_t ts, TrackEventData data) override;
  void ParseTracePacket(int64_t ts, TracePacketData data) override;

  void ParseFtraceEvent(uint32_t cpu,
                        int64_t /*ts*/,
                        TracePacketData data) override;

  void ParseInlineSchedSwitch(uint32_t cpu,
                              int64_t /*ts*/,
                              InlineSchedSwitch data) override;

  void ParseInlineSchedWaking(uint32_t cpu,
                              int64_t /*ts*/,
                              InlineSchedWaking data) override;

  void ParseTraceStats(ConstBytes);
  void ParseChromeEvents(int64_t ts, ConstBytes);
  void ParseMetatraceEvent(int64_t ts, ConstBytes);

 private:
  StringId GetMetatraceInternedString(uint64_t iid);

  TraceProcessorContext* context_;

  const StringId metatrace_id_;
  const StringId data_name_id_;
  const StringId raw_chrome_metadata_event_id_;
  const StringId raw_chrome_legacy_system_trace_event_id_;
  const StringId raw_chrome_legacy_user_trace_event_id_;
  const StringId missing_metatrace_interned_string_id_;

  base::FlatHashMap<uint64_t, StringId> metatrace_interned_strings_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_PARSER_H_
