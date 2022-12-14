/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/importers/common/trace_parser.h"

#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"

namespace perfetto {
namespace trace_processor {

void TraceParser::ParseTracePacket(int64_t, TracePacketData) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseJsonPacket(int64_t, std::string) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseFuchsiaRecord(int64_t, FuchsiaRecord) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseTrackEvent(int64_t, TrackEventData) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseSystraceLine(int64_t, SystraceLine) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseFtraceEvent(uint32_t, int64_t, TracePacketData) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseInlineSchedSwitch(uint32_t, int64_t, InlineSchedSwitch) {
  PERFETTO_FATAL("Wrong parser type");
}
void TraceParser::ParseInlineSchedWaking(uint32_t, int64_t, InlineSchedWaking) {
  PERFETTO_FATAL("Wrong parser type");
}

}  // namespace trace_processor
}  // namespace perfetto
