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

#ifndef TOOLS_TRACE_TO_TEXT_FTRACE_EVENT_FORMATTER_H_
#define TOOLS_TRACE_TO_TEXT_FTRACE_EVENT_FORMATTER_H_

#include "tools/trace_to_text/ftrace_event_formatter.h"

#include <string>
#include <unordered_map>

#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {

std::string FormatFtraceEvent(
    uint64_t timestamp,
    uint32_t cpu,
    const protos::FtraceEvent&,
    const std::unordered_map<uint32_t /*tid*/, uint32_t /*tgid*/>& thread_map,
    std::unordered_map<uint32_t /*tid*/, std::string>& thread_names);

}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_FTRACE_EVENT_FORMATTER_H_
