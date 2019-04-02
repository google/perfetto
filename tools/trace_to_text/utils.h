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

#ifndef TOOLS_TRACE_TO_TEXT_UTILS_H_
#define TOOLS_TRACE_TO_TEXT_UTILS_H_

#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <memory>

#include "perfetto/base/build_config.h"

namespace perfetto {

namespace protos {
class TracePacket;
}

namespace trace_to_text {

// When running in Web Assembly, fflush() is a no-op and the stdio buffering
// sends progress updates to JS only when a write ends with \n.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
constexpr char kProgressChar = '\n';
#else
constexpr char kProgressChar = '\r';
#endif

void ForEachPacketBlobInTrace(
    std::istream* input,
    const std::function<void(std::unique_ptr<char[]>, size_t)>&);

void ForEachPacketInTrace(
    std::istream* input,
    const std::function<void(const protos::TracePacket&)>&);

}  // namespace trace_to_text
}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_UTILS_H_
