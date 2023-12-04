/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACE_PARSER_H_

#include <stdint.h>
#include <string>

namespace perfetto {
namespace trace_processor {

class PacketSequenceStateGeneration;
class TraceBlobView;
struct InlineSchedSwitch;
class FuchsiaRecord;
struct SystraceLine;
struct InlineSchedWaking;
struct TracePacketData;
struct TrackEventData;

class TraceParser {
 public:
  virtual ~TraceParser();

  virtual void ParseTraceBlobView(int64_t, TraceBlobView);
  virtual void ParseTracePacket(int64_t, TracePacketData);
  virtual void ParseJsonPacket(int64_t, std::string);
  virtual void ParseFuchsiaRecord(int64_t, FuchsiaRecord);
  virtual void ParseTrackEvent(int64_t, TrackEventData);
  virtual void ParseSystraceLine(int64_t, SystraceLine);

  virtual void ParseFtraceEvent(uint32_t, int64_t, TracePacketData);
  virtual void ParseInlineSchedSwitch(uint32_t, int64_t, InlineSchedSwitch);
  virtual void ParseInlineSchedWaking(uint32_t, int64_t, InlineSchedWaking);
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACE_PARSER_H_
